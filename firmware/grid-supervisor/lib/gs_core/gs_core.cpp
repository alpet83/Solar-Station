#include "gs_core.h"

#include <algorithm>
#include <cmath>

namespace gs {

namespace {

inline bool finite(float v) { return std::isfinite(v); }

// Разность моментов времени, устойчивая к «времени назад» (возвращает 0, а не огромное число).
inline uint64_t since(uint64_t now, uint64_t then) { return now >= then ? now - then : 0; }

}  // namespace

bool Config::validate() const {
    const float fv[] = {hard_floor_v, soft_on_v, soft_off_v, full_v, pow_min_base_w, load_factor,
                        ext_coef, ext_max_a, charge_max_a, v_valid_min, v_valid_max,
                        v_mismatch_max, grid_present_v_min};
    for (float f : fv)
        if (!finite(f)) return false;

    if (!(v_valid_min < hard_floor_v)) return false;
    if (!(soft_on_v - hard_floor_v >= 0.5f)) return false;
    if (!(soft_off_v - soft_on_v >= 0.5f)) return false;
    if (!(full_v >= soft_off_v)) return false;
    if (!(full_v <= v_valid_max)) return false;

    if (!(pow_min_base_w > 0.0f)) return false;          // иначе 0 А «достаточно» для отключения
    if (load_factor < 0.0f || load_factor > 2.0f) return false;
    if (ext_coef < 0.0f || ext_coef > 1.0f) return false;
    if (ext_max_a <= 0.0f || ext_max_a > 500.0f) return false;
    if (charge_max_a <= 0.0f || charge_max_a > 500.0f) return false;
    if (v_mismatch_max <= 0.0f || v_mismatch_max > 5.0f) return false;

    // Времена, ведущие к подключению сети, ограничены СВЕРХУ (иначе защиту можно «выключить»
    // конфигом); времена, ведущие к отключению, ограничены СНИЗУ.
    if (hard_dwell_ms > 60000u) return false;
    if (soft_on_dwell_ms == 0 || soft_on_dwell_ms > 900000u) return false;
    if (no_data_timeout_ms < 10000u || no_data_timeout_ms > 600000u) return false;
    if (soft_off_dwell_ms < 60000u) return false;
    if (hold_after_connect_ms < 30000u || hold_after_connect_ms > 7200000u) return false;
    if (manual_pause_ms > 86400000u) return false;
    if (cmd_retry_ms < 1000u || cmd_retry_ms > 120000u) return false;
    if (ext_ttl_ms < 5000u || ext_ttl_ms > 300000u) return false;
    if (step_gap_ms < 2000u || step_gap_ms > 300000u) return false;
    return true;
}

float effective_charge_a(const Config& cfg, float charge_a, float ext_a, uint64_t ext_age_ms) {
    // Неправдоподобный собственный ток (мусор по шине) = 0: сторона «меньше заряда» = сеть.
    const float own = (finite(charge_a) && charge_a > 0.0f && charge_a <= cfg.charge_max_a) ? charge_a : 0.0f;
    float ext = 0.0f;
    if (finite(ext_a) && ext_a > 0.0f && ext_age_ms <= cfg.ext_ttl_ms)
        ext = std::min(ext_a, cfg.ext_max_a);
    return own + cfg.ext_coef * ext;
}

Decision Supervisor::step(const Input& in) {
    Decision d;
    const uint64_t now = in.now_ms;
    uint32_t flags = 0;

    // ---- 0. время: старт, откат/переполнение, пропуск шагов ---------------------------------
    if (!started_) {
        started_ = true;
        start_ms_ = now;
        last_valid_ms_ = now;  // отсчёт «нет данных» идёт от старта
        // После загрузки считаем сеть «только что подключённой»: перезагрузка не должна
        // тут же приводить к отключению (hold), а потеря RAM — стирать недавние решения.
        last_connect_ms_ = static_cast<int64_t>(now);
    } else if (have_last_step_ && now < last_step_ms_) {
        // Время пошло назад (переполнение millis, смена источника времени): все отметки
        // «с какого момента» перезапускаем от текущего момента, паузу ограничиваем сверху.
        flags |= F_TIME_JUMP;
        last_valid_ms_ = now;
        if (hard_since_ >= 0) hard_since_ = static_cast<int64_t>(now);
        if (soft_since_ >= 0) soft_since_ = static_cast<int64_t>(now);
        off_since_ = kInactive;
        pending_since_ = now;
        if (last_connect_ms_ >= 0) last_connect_ms_ = static_cast<int64_t>(now);
        if (last_cmd_conn_ms_ >= 0) last_cmd_conn_ms_ = static_cast<int64_t>(now);
        if (last_cmd_disc_ms_ >= 0) last_cmd_disc_ms_ = static_cast<int64_t>(now);
    }
    if (pause_until_ms_ > now + cfg_.manual_pause_ms) pause_until_ms_ = now + cfg_.manual_pause_ms;
    if (have_last_step_ && now >= last_step_ms_ && now - last_step_ms_ > cfg_.step_gap_ms) {
        // Шаги не вызывались долго: непрерывность условия «заряжено» не доказана.
        flags |= F_TIME_JUMP;
        off_since_ = kInactive;
    }
    have_last_step_ = true;
    last_step_ms_ = now;

    // ---- 1. валидность измерений ------------------------------------------------------------
    auto volt_ok = [&](float x) { return finite(x) && x >= cfg_.v_valid_min && x <= cfg_.v_valid_max; };
    const bool a_ok = volt_ok(in.pack_v);
    const bool b_ok = volt_ok(in.pack_v_alt);  // 0 В и NaN отсекаются диапазоном
    const bool fresh = in.have_sample && (a_ok || b_ok);

    bool doubtful = false, single = false;
    float v = last_v_;
    if (fresh) {
        if (a_ok && b_ok) {
            v = std::min(in.pack_v, in.pack_v_alt);
            doubtful = std::fabs(in.pack_v - in.pack_v_alt) > cfg_.v_mismatch_max;
        } else {
            v = a_ok ? in.pack_v : in.pack_v_alt;
            single = true;
        }
        last_v_ = v;
        last_valid_ms_ = now;
        if (finite(in.load_w) && in.load_w >= 0.0f) last_load_w_ = in.load_w;
    }
    const bool load_known = fresh && finite(in.load_w) && in.load_w >= 0.0f;
    const bool no_data = !fresh && since(now, last_valid_ms_) >= cfg_.no_data_timeout_ms;

    // ---- 2. регистр 20105: «наше» изменение или внешнее --------------------------------------
    // Изменение считается нашим, если соответствующая команда выдавалась недавно (окно
    // покрывает несколько повторов и задержку применения). Это защищает и от ложной паузы
    // при перекрывающихся командах, и от «подтверждения» чужого действия давней командой.
    const uint64_t own_window = static_cast<uint64_t>(cfg_.cmd_retry_ms) * 3;
    auto recent = [&](int64_t t) { return t >= 0 && since(now, static_cast<uint64_t>(t)) <= own_window; };
    const int8_t reg = in.ongrid_reg;
    bool manual_now = false;
    if (reg == 0 || reg == 1) {
        if (last_reg_ >= 0 && reg != last_reg_) {
            const bool ours = reg == 1 ? recent(last_cmd_conn_ms_) : recent(last_cmd_disc_ms_);
            if (!ours) {
                manual_now = true;
                pause_until_ms_ = now + cfg_.manual_pause_ms;
            }
            if (reg == 1) last_connect_ms_ = static_cast<int64_t>(now);  // hold от факта подключения
        }
        if (pending_ != Cmd::None && reg == (pending_ == Cmd::Connect ? 1 : 0)) pending_ = Cmd::None;
        last_reg_ = reg;
    }

    // ---- 3. таймеры условий ------------------------------------------------------------------
    const float eff = effective_charge_a(cfg_, in.charge_a, in.ext_charge_a, in.ext_age_ms);
    const float load_for_pow = load_known ? in.load_w : last_load_w_;
    const float pow_min = std::max(cfg_.pow_min_base_w, load_for_pow * cfg_.load_factor);

    auto track = [&](int64_t& since_ms, bool cond) {
        if (cond) {
            if (since_ms < 0) since_ms = static_cast<int64_t>(now);
        } else {
            since_ms = kInactive;
        }
    };

    if (fresh) {
        track(hard_since_, v <= cfg_.hard_floor_v);
        track(soft_since_, v <= cfg_.soft_on_v);
        const bool by_power = load_known && v >= cfg_.soft_off_v && eff * v >= pow_min;
        const bool by_full = !single && v >= cfg_.full_v;   // «полный» без проверки мощности — только по двум источникам
        track(off_since_, reg == 1 && !doubtful && (by_power || by_full));
    } else {
        // Нет свежих данных: «низкие» таймеры НЕ сбрасываем (последнее состояние считаем
        // продолжающимся — сторона сети), а «заряжено» обнуляем сразу.
        off_since_ = kInactive;
    }

    // ---- 4. решение --------------------------------------------------------------------------
    const bool paused = now < pause_until_ms_;
    const bool hard_trip = hard_since_ >= 0 && since(now, static_cast<uint64_t>(hard_since_)) >= cfg_.hard_dwell_ms;
    const bool soft_trip = soft_since_ >= 0 && since(now, static_cast<uint64_t>(soft_since_)) >= cfg_.soft_on_dwell_ms;
    const bool off_trip  = off_since_ >= 0 && since(now, static_cast<uint64_t>(off_since_)) >= cfg_.soft_off_dwell_ms;
    const bool hold = last_connect_ms_ >= 0 && since(now, static_cast<uint64_t>(last_connect_ms_)) < cfg_.hold_after_connect_ms;

    if (hard_since_ >= 0) flags |= F_HARD_LOW;
    if (soft_since_ >= 0) flags |= F_SOFT_LOW;
    if (no_data) flags |= F_NO_DATA;
    if (!fresh) flags |= F_STALE;
    if (doubtful) flags |= F_DATA_DOUBTFUL;
    if (single) flags |= F_SINGLE_SRC;
    if (fresh && !load_known) flags |= F_LOAD_UNKNOWN;
    if (paused) flags |= F_MANUAL_PAUSE;
    if (manual_now) flags |= F_MANUAL_OVERRIDE;
    if (in.safe_mode) flags |= F_SAFE_MODE;
    if (off_since_ >= 0) flags |= F_OFF_COND;
    if (hold) flags |= F_HOLD;
    if (fresh && finite(in.grid_v) && in.grid_v < cfg_.grid_present_v_min && v <= cfg_.soft_on_v)
        flags |= F_GRID_LOST_LOW;

    Cmd want = Cmd::None;
    Reason why = Reason::None;
    if (hard_trip) {
        want = Cmd::Connect; why = Reason::HardFloor;                // ничто не отменяет
    } else if (in.safe_mode) {
        want = Cmd::Connect; why = Reason::SafeMode;                 // петля перезагрузок: сразу к сети
    } else if (no_data) {
        want = Cmd::Connect; why = Reason::NoData;                   // ничто не отменяет
    } else if (!paused && soft_trip) {
        want = Cmd::Connect; why = Reason::SoftLow;
    } else if (!paused && fresh && off_trip && !hold && reg == 1 && !doubtful) {
        want = Cmd::Disconnect; why = Reason::Charged;
    }

    // Устаревшее ожидание: желание изменилось — старый pending не должен «подтверждаться» позже.
    if (pending_ != Cmd::None && want != pending_) pending_ = Cmd::None;

    // Connect нужен, если сеть не подтверждена подключённой ИЛИ ещё висит наш Disconnect.
    Cmd issue = Cmd::None;
    const bool disc_recent = recent(last_cmd_disc_ms_);
    if (want == Cmd::Connect && (reg != 1 || disc_recent))
        issue = Cmd::Connect;
    else if (want == Cmd::Disconnect) issue = Cmd::Disconnect;

    // ---- 5. ограничение частоты команд ------------------------------------------------------
    if (issue != Cmd::None) {
        if (pending_ == issue) {
            if (since(now, pending_since_) < cfg_.cmd_retry_ms) {
                issue = Cmd::None;               // уже выдана, ждём подтверждения
                flags |= F_CMD_PENDING;
            } else {
                pending_since_ = now;            // повтор неподтверждённой команды
                flags |= F_CMD_RETRY;
            }
        } else {
            pending_ = issue;
            pending_since_ = now;
        }
    } else if (pending_ != Cmd::None) {
        flags |= F_CMD_PENDING;
    }

    if (issue == Cmd::Connect) {
        last_cmd_conn_ms_ = static_cast<int64_t>(now);
        if (!(flags & F_CMD_RETRY)) last_connect_ms_ = static_cast<int64_t>(now);
    } else if (issue == Cmd::Disconnect) {
        last_cmd_disc_ms_ = static_cast<int64_t>(now);
    }

    d.cmd = issue;
    d.reason = (issue == Cmd::None) ? Reason::None : why;
    d.flags = flags;
    d.v_used = v;
    d.eff_charge_a = eff;
    d.pow_min_w = pow_min;
    return d;
}

}  // namespace gs
