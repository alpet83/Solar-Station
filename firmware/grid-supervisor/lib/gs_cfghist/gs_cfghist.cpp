#include "gs_cfghist.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "gs_util.h"

namespace gs {

namespace {
struct W {
    char* b; size_t cap, n; bool ok;
    void put(const char* s) { size_t l = strlen(s); if (n + l >= cap) { ok = false; return; } memcpy(b + n, s, l); n += l; b[n] = 0; }
    void putn(const char* s, size_t l) { if (n + l >= cap) { ok = false; return; } memcpy(b + n, s, l); n += l; b[n] = 0; }
};

// Находит "name":{...} (сбалансированные скобки, внутри нет строк со скобками). p/n — содержимое вместе с { }.
bool section(const char* s, size_t len, const char* name, const char*& p, size_t& n) {
    char pat[40]; snprintf(pat, sizeof pat, "\"%s\":{", name);
    const size_t pl = strlen(pat);
    for (size_t i = 0; i + pl <= len; ++i) {
        if (memcmp(s + i, pat, pl) != 0) continue;
        size_t j = i + pl - 1; int depth = 0;
        for (size_t k = j; k < len; ++k) {
            if (s[k] == '{') ++depth;
            else if (s[k] == '}') { if (--depth == 0) { p = s + j; n = k - j + 1; return true; } }
        }
        return false;
    }
    return false;
}

bool num_after(const char* s, size_t len, const char* name, long long& v) {
    char pat[24]; snprintf(pat, sizeof pat, "\"%s\":", name);
    const size_t pl = strlen(pat);
    for (size_t i = 0; i + pl < len; ++i) {
        if (memcmp(s + i, pat, pl) != 0) continue;
        char tmp[24]; size_t k = 0;
        for (size_t j = i + pl; j < len && k < sizeof tmp - 1 && ((s[j] >= '0' && s[j] <= '9') || s[j] == '-'); ++j) tmp[k++] = s[j];
        if (!k) return false;
        tmp[k] = 0; v = atoll(tmp); return true;
    }
    return false;
}
}  // namespace

size_t cfg_hist_line(const Config& before, const Config& after, int64_t epoch_s, uint32_t id, const char* src,
                     bool force, char* out, size_t cap) {
    if (!out || cap < 128) return 0;
    char s[32]; size_t sl = 0;
    for (const char* p = src ? src : ""; *p && sl < sizeof s - 1; ++p)
        s[sl++] = ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == ':' || *p == '_' || *p == '-') ? *p : '_';
    s[sl] = 0;
    W w{out, cap, 0, true}; out[0] = 0;
    char t[128];
    snprintf(t, sizeof t, "{\"id\":%lu,\"t\":%lld,\"src\":\"%s\",\"ch\":{", (unsigned long)id, (long long)(epoch_s > 0 ? epoch_s : 0), s);
    w.put(t);
    size_t changed = 0;
    for (size_t i = 0; i < kCfgFieldCount; ++i) {
        char a[24], b[24];
        config_get(before, kCfgFields[i].key, a, sizeof a); config_get(after, kCfgFields[i].key, b, sizeof b);
        if (!strcmp(a, b)) continue;
        snprintf(t, sizeof t, "%s\"%s\":[%s,%s]", changed ? "," : "", kCfgFields[i].key, a, b);
        w.put(t); ++changed;
    }
    if (!changed && !force) return 0;
    w.put("},\"cfg\":");
    char* dst = out + w.n;
    size_t n = config_to_json(after, dst, cap - w.n);
    if (!n) return 0;
    w.n += n;
    w.put("}\n");
    return w.ok ? w.n : 0;
}

bool cfg_hist_header(const char* line, size_t len, CfgHistHeader& h) {
    long long id, t;
    if (!line || len < 10 || line[0] != '{') return false;
    if (!num_after(line, len, "id", id) || !num_after(line, len, "t", t) || id < 0) return false;
    h.id = (uint32_t)id; h.t = t;
    const char* pat = "\"src\":\"";
    h.src[0] = 0;
    for (size_t i = 0; i + 7 < len; ++i) {
        if (memcmp(line + i, pat, 7) != 0) continue;
        size_t k = 0;
        for (size_t j = i + 7; j < len && line[j] != '"' && k < sizeof h.src - 1; ++j) h.src[k++] = line[j];
        h.src[k] = 0; break;
    }
    return true;
}

size_t cfg_hist_summary(const char* line, size_t len, char* out, size_t cap) {
    CfgHistHeader h;
    if (!cfg_hist_header(line, len, h) || !out) return 0;
    const char* ch = "{}"; size_t cl = 2;
    const char* p; size_t n;
    if (section(line, len, "ch", p, n)) { ch = p; cl = n; }
    W w{out, cap, 0, true}; out[0] = 0;
    char t[96];
    snprintf(t, sizeof t, "{\"id\":%lu,\"t\":%lld,\"src\":\"%s\",\"ch\":", (unsigned long)h.id, (long long)h.t, h.src);
    w.put(t); w.putn(ch, cl); w.put("}");
    return w.ok ? w.n : 0;
}

bool cfg_hist_apply(const char* line, size_t len, Config& cfg) {
    const char* p; size_t n;
    if (!section(line, len, "cfg", p, n)) return false;
    const char* keys[32]; const char* vals[32]; char buf[32][24]; size_t cnt = 0;
    for (size_t i = 0; i < kCfgFieldCount && cnt < 32; ++i) {
        char pat[40]; snprintf(pat, sizeof pat, "\"%s\":", kCfgFields[i].key);
        const size_t pl = strlen(pat);
        for (size_t k = 0; k + pl < n; ++k) {
            if (memcmp(p + k, pat, pl) != 0) continue;
            size_t m = 0;
            for (size_t j = k + pl; j < n && p[j] != ',' && p[j] != '}' && m < 23; ++j) buf[cnt][m++] = p[j];
            buf[cnt][m] = 0;
            if (!m) return false;
            keys[cnt] = kCfgFields[i].key; vals[cnt] = buf[cnt]; ++cnt;
            break;
        }
    }
    if (!cnt) return false;
    return config_apply_pairs(cfg, keys, vals, cnt);
}

}  // namespace gs
