#pragma once
// ============================================================================
// json.hpp —— 极简 JSON 解析/生成（够 MCP JSON-RPC 用，零依赖）
// ============================================================================
// 只实现 MCP 需要的子集：object/array/string/number/bool/null。
// 解析用递归下降，生成用手写转义。不做 Unicode 转义解码（MCP 里用不到）。
// ============================================================================

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace mj {

class Value;
using Array  = std::vector<Value>;
using Object = std::map<std::string, Value>;

enum class Type { Null, Bool, Num, Str, Arr, Obj };

class Value
{
public:
    Type type = Type::Null;

    bool                       b = false;
    double                     num = 0;
    std::string                str;
    std::shared_ptr<Array>     arr;
    std::shared_ptr<Object>    obj;

    Value() = default;
    Value(bool v) : type(Type::Bool), b(v) {}
    Value(int v) : type(Type::Num), num((double)v) {}
    Value(long long v) : type(Type::Num), num((double)v) {}
    Value(unsigned long long v) : type(Type::Num), num((double)v) {}
    Value(double v) : type(Type::Num), num(v) {}
    Value(const char *v) : type(Type::Str), str(v) {}
    Value(const std::string &v) : type(Type::Str), str(v) {}

    static Value make_arr() { Value v; v.type = Type::Arr; v.arr = std::make_shared<Array>(); return v; }
    static Value make_obj() { Value v; v.type = Type::Obj; v.obj = std::make_shared<Object>(); return v; }

    bool is_null() const { return type == Type::Null; }
    bool is_arr()  const { return type == Type::Arr; }
    bool is_obj()  const { return type == Type::Obj; }

    // arr 操作
    void push(const Value &v) { if (!arr) arr = std::make_shared<Array>(); arr->push_back(v); }
    size_t size() const { return arr ? arr->size() : 0; }
    const Value &at(size_t i) const { return (*arr)[i]; }

    // obj 操作
    void set(const std::string &k, const Value &v) { if (!obj) obj = std::make_shared<Object>(); (*obj)[k] = v; }
    bool has(const std::string &k) const { return obj && obj->count(k) > 0; }
    const Value &get(const std::string &k) const
    {
        static Value null_v;
        if (!obj) return null_v;
        auto it = obj->find(k);
        return it == obj->end() ? null_v : it->second;
    }

    std::string as_str(const std::string &def = "") const { return type == Type::Str ? str : def; }
    double      as_num(double def = 0) const { return type == Type::Num ? num : def; }
    long long   as_int(long long def = 0) const { return type == Type::Num ? (long long)num : def; }
    bool        as_bool(bool def = false) const { return type == Type::Bool ? b : def; }

    // ── 序列化 ────────────────────────────────────────────────────────────
    std::string dump() const
    {
        std::string o;
        write(o);
        return o;
    }

private:
    static void esc(std::string &o, const std::string &s)
    {
        o += '"';
        for (char c : s)
        {
            switch (c)
            {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    o += buf;
                }
                else
                    o += c;
            }
        }
        o += '"';
    }

    void write(std::string &o) const
    {
        switch (type)
        {
        case Type::Null: o += "null"; break;
        case Type::Bool: o += b ? "true" : "false"; break;
        case Type::Num:
        {
            // 整数不带小数点，避免 5 输出成 5.0
            if (num == std::floor(num) && std::fabs(num) < 1e15)
            {
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", (long long)num);
                o += buf;
            }
            else
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.10g", num);
                o += buf;
            }
            break;
        }
        case Type::Str: esc(o, str); break;
        case Type::Arr:
        {
            o += '[';
            if (arr)
                for (size_t i = 0; i < arr->size(); ++i)
                {
                    if (i) o += ',';
                    (*arr)[i].write(o);
                }
            o += ']';
            break;
        }
        case Type::Obj:
        {
            o += '{';
            if (obj)
            {
                bool first = true;
                for (auto &kv : *obj)
                {
                    if (!first) o += ',';
                    first = false;
                    esc(o, kv.first);
                    o += ':';
                    kv.second.write(o);
                }
            }
            o += '}';
            break;
        }
        }
    }
};

// ── 解析器 ───────────────────────────────────────────────────────────────
class Parser
{
public:
    explicit Parser(const std::string &s) : s_(s) {}

    bool parse(Value &out)
    {
        skip();
        if (!value(out))
            return false;
        skip();
        return true;
    }

private:
    const std::string &s_;
    size_t i_ = 0;

    void skip()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
            ++i_;
    }

    bool value(Value &out)
    {
        skip();
        if (i_ >= s_.size())
            return false;
        char c = s_[i_];
        if (c == '{') return object(out);
        if (c == '[') return array(out);
        if (c == '"') { std::string t; if (!string(t)) return false; out = Value(t); return true; }
        if (c == 't') { if (s_.compare(i_, 4, "true") == 0) { i_ += 4; out = Value(true); return true; } return false; }
        if (c == 'f') { if (s_.compare(i_, 5, "false") == 0) { i_ += 5; out = Value(false); return true; } return false; }
        if (c == 'n') { if (s_.compare(i_, 4, "null") == 0) { i_ += 4; out = Value(); return true; } return false; }
        return number(out);
    }

    bool number(Value &out)
    {
        size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+'))
            ++i_;
        while (i_ < s_.size() && (isdigit((unsigned char)s_[i_]) || s_[i_] == '.' ||
                                  s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '-' || s_[i_] == '+'))
            ++i_;
        if (i_ == start)
            return false;
        out = Value(strtod(s_.substr(start, i_ - start).c_str(), nullptr));
        return true;
    }

    bool string(std::string &out)
    {
        if (s_[i_] != '"')
            return false;
        ++i_;
        out.clear();
        while (i_ < s_.size())
        {
            char c = s_[i_++];
            if (c == '"')
                return true;
            if (c == '\\')
            {
                if (i_ >= s_.size())
                    return false;
                char e = s_[i_++];
                switch (e)
                {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u':
                {
                    if (i_ + 4 > s_.size())
                        return false;
                    unsigned cp = strtoul(s_.substr(i_, 4).c_str(), nullptr, 16);
                    i_ += 4;
                    // UTF-8 编码（BMP）
                    if (cp < 0x80)
                        out += (char)cp;
                    else if (cp < 0x800)
                    {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    else
                    {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: out += e;
                }
            }
            else
                out += c;
        }
        return false;
    }

    bool object(Value &out)
    {
        out = Value::make_obj();
        ++i_; // {
        skip();
        if (i_ < s_.size() && s_[i_] == '}')
        {
            ++i_;
            return true;
        }
        while (i_ < s_.size())
        {
            skip();
            std::string key;
            if (!string(key))
                return false;
            skip();
            if (i_ >= s_.size() || s_[i_] != ':')
                return false;
            ++i_;
            Value v;
            if (!value(v))
                return false;
            out.set(key, v);
            skip();
            if (i_ < s_.size() && s_[i_] == ',')
            {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == '}')
            {
                ++i_;
                return true;
            }
            return false;
        }
        return false;
    }

    bool array(Value &out)
    {
        out = Value::make_arr();
        ++i_; // [
        skip();
        if (i_ < s_.size() && s_[i_] == ']')
        {
            ++i_;
            return true;
        }
        while (i_ < s_.size())
        {
            Value v;
            if (!value(v))
                return false;
            out.push(v);
            skip();
            if (i_ < s_.size() && s_[i_] == ',')
            {
                ++i_;
                continue;
            }
            if (i_ < s_.size() && s_[i_] == ']')
            {
                ++i_;
                return true;
            }
            return false;
        }
        return false;
    }
};

inline bool parse(const std::string &s, Value &out)
{
    Parser p(s);
    return p.parse(out);
}

} // namespace mj
