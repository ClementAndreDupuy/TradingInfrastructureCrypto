#pragma once
// Vendored minimal nlohmann-compatible JSON parser. No external dependencies.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nlohmann {

class json {
  public:
    enum class value_t : uint8_t {
        null,
        boolean,
        number_integer,
        number_unsigned,
        number_float,
        string,
        array,
        object,
        discarded
    };

    using array_t = std::vector<json>;
    using object_t = std::vector<std::pair<std::string, json>>;

    json() : type_(value_t::null) {}
    json(std::nullptr_t) : type_(value_t::null) {}
    json(bool v) : type_(value_t::boolean) { b_ = v; }
    json(int64_t v) : type_(value_t::number_integer) { i_ = v; }
    json(uint64_t v) : type_(value_t::number_unsigned) { u_ = v; }
    json(double v) : type_(value_t::number_float) { d_ = v; }
    json(const std::string& v) : type_(value_t::string), s_(v) {}
    json(std::string&& v) : type_(value_t::string), s_(std::move(v)) {}
    json(std::initializer_list<json> init) {
        bool all_pairs = true;
        for (const auto& elem : init) {
            if (!elem.is_array() || !elem.arr_ || elem.arr_->size() != 2 ||
                !(*elem.arr_)[0].is_string()) {
                all_pairs = false;
                break;
            }
        }
        if (all_pairs) {
            type_ = value_t::object;
            obj_ = new object_t();
            for (const auto& elem : init)
                obj_->emplace_back((*elem.arr_)[0].s_, (*elem.arr_)[1]);
        } else {
            type_ = value_t::array;
            arr_ = new array_t(init);
        }
    }

    json(const json& o) { copy_from(o); }
    json(json&& o) noexcept { move_from(std::move(o)); }

    json& operator=(const json& o) {
        if (this != &o) {
            clear();
            copy_from(o);
        }
        return *this;
    }
    json& operator=(json&& o) noexcept {
        if (this != &o) {
            clear();
            move_from(std::move(o));
        }
        return *this;
    }

    ~json() { clear(); }

    // ── Factories ───────────────────────────────────────────────────────────
    static json array() {
        json j;
        j.type_ = value_t::array;
        j.arr_ = new array_t();
        return j;
    }
    static json array(std::initializer_list<json> init) {
        json j;
        j.type_ = value_t::array;
        j.arr_ = new array_t(init);
        return j;
    }
    static json object() {
        json j;
        j.type_ = value_t::object;
        return j;
    }

    bool is_null() const noexcept { return type_ == value_t::null; }
    bool is_boolean() const noexcept { return type_ == value_t::boolean; }
    bool is_number_integer() const noexcept { return type_ == value_t::number_integer; }
    bool is_number_unsigned() const noexcept { return type_ == value_t::number_unsigned; }
    bool is_number_float() const noexcept { return type_ == value_t::number_float; }
    bool is_number() const noexcept {
        return type_ == value_t::number_integer || type_ == value_t::number_unsigned ||
               type_ == value_t::number_float;
    }
    bool is_string() const noexcept { return type_ == value_t::string; }
    bool is_array() const noexcept { return type_ == value_t::array; }
    bool is_object() const noexcept { return type_ == value_t::object; }
    bool is_discarded() const noexcept { return type_ == value_t::discarded; }

    size_t size() const noexcept {
        if (is_array())
            return arr_ ? arr_->size() : 0;
        if (is_object())
            return obj_ ? obj_->size() : 0;
        return 0;
    }
    bool empty() const noexcept { return size() == 0; }

    template <typename T> T get() const;

    const json& operator[](size_t idx) const {
        static const json null_j;
        if (!is_array() || !arr_ || idx >= arr_->size())
            return null_j;
        return (*arr_)[idx];
    }
    const json& operator[](int idx) const { return operator[](static_cast<size_t>(idx)); }
    const json& operator[](const char* key) const { return at_key(key); }
    const json& operator[](const std::string& key) const { return at_key(key); }

    json& operator[](const char* key) { return at_key_mut(std::string(key)); }
    json& operator[](const std::string& key) { return at_key_mut(key); }

    class iterator {
      public:
        iterator() = default;
        explicit iterator(const json* j, size_t idx) : j_(j), idx_(idx) {}

        bool operator==(const iterator& o) const { return j_ == o.j_ && idx_ == o.idx_; }
        bool operator!=(const iterator& o) const { return !(*this == o); }

        iterator& operator++() {
            ++idx_;
            return *this;
        }

        const json& operator*() const { return deref(); }
        const json* operator->() const { return &deref(); }

        // For object iterators
        const std::string& key() const {
            static const std::string empty_key;
            if (j_ && j_->is_object() && j_->obj_ && idx_ < j_->obj_->size())
                return (*j_->obj_)[idx_].first;
            return empty_key;
        }
        const json& value() const { return deref(); }

      private:
        const json* j_{nullptr};
        size_t idx_{0};

        const json& deref() const {
            static const json null_j;
            if (!j_)
                return null_j;
            if (j_->is_array() && j_->arr_ && idx_ < j_->arr_->size())
                return (*j_->arr_)[idx_];
            if (j_->is_object() && j_->obj_ && idx_ < j_->obj_->size())
                return (*j_->obj_)[idx_].second;
            return null_j;
        }
    };

    iterator begin() const {
        if ((is_array() && arr_) || (is_object() && obj_))
            return iterator(this, 0);
        return end();
    }
    iterator end() const { return iterator(this, size()); }

    // ── find ────────────────────────────────────────────────────────────────
    bool contains(const std::string& key) const { return find(key) != end(); }
    bool contains(const char* key) const { return find(key) != end(); }

    iterator find(const std::string& key) const {
        if (is_object() && obj_) {
            for (size_t i = 0; i < obj_->size(); ++i) {
                if ((*obj_)[i].first == key)
                    return iterator(this, i);
            }
        }
        return end();
    }
    iterator find(const char* key) const { return find(std::string(key)); }

    // ── value(key, default) ─────────────────────────────────────────────────
    const json& value(const std::string& key, const json& def) const {
        auto it = find(key);
        return (it != end()) ? *it : def;
    }

    template <typename T> T value(const std::string& key, const T& def) const {
        auto it = find(key);
        if (it == end())
            return def;
        try {
            return it->get<T>();
        } catch (...) {
            return def;
        }
    }

    // ── parse ────────────────────────────────────────────────────────────────
    // If allow_exceptions=false: returns discarded value on parse error.
    static json parse(const std::string& s, std::nullptr_t /*cb*/ = nullptr,
                      bool allow_exceptions = true) {
        const char* p = s.c_str();
        const char* end = p + s.size();
        try {
            Parser pr(p, end);
            json result = pr.parse_value();
            pr.skip_ws();
            if (pr.pos != end)
                throw std::invalid_argument("trailing chars");
            return result;
        } catch (...) {
            if (allow_exceptions)
                throw;
            json d;
            d.type_ = value_t::discarded;
            return d;
        }
    }

    // ── dump ─────────────────────────────────────────────────────────────────
    std::string dump(int /*indent*/ = -1) const {
        std::ostringstream os;
        serialize(os);
        return os.str();
    }

  private:
    // ── Storage ─────────────────────────────────────────────────────────────
    value_t type_{value_t::null};
    std::string s_; // string value
    union {
        bool b_;
        int64_t i_;
        uint64_t u_;
        double d_;
    };
    array_t* arr_{nullptr}; // heap-allocated to keep sizeof small
    object_t* obj_{nullptr};

    void clear() noexcept {
        if (arr_) {
            delete arr_;
            arr_ = nullptr;
        }
        if (obj_) {
            delete obj_;
            obj_ = nullptr;
        }
        type_ = value_t::null;
        s_.clear();
    }

    void copy_from(const json& o) {
        type_ = o.type_;
        s_ = o.s_;
        switch (type_) {
        case value_t::boolean:
            b_ = o.b_;
            break;
        case value_t::number_integer:
            i_ = o.i_;
            break;
        case value_t::number_unsigned:
            u_ = o.u_;
            break;
        case value_t::number_float:
            d_ = o.d_;
            break;
        case value_t::array:
            if (o.arr_)
                arr_ = new array_t(*o.arr_);
            break;
        case value_t::object:
            if (o.obj_)
                obj_ = new object_t(*o.obj_);
            break;
        default:
            break;
        }
    }

    void move_from(json&& o) noexcept {
        type_ = o.type_;
        s_ = std::move(o.s_);
        switch (type_) {
        case value_t::boolean:
            b_ = o.b_;
            break;
        case value_t::number_integer:
            i_ = o.i_;
            break;
        case value_t::number_unsigned:
            u_ = o.u_;
            break;
        case value_t::number_float:
            d_ = o.d_;
            break;
        case value_t::array:
            arr_ = o.arr_;
            o.arr_ = nullptr;
            break;
        case value_t::object:
            obj_ = o.obj_;
            o.obj_ = nullptr;
            break;
        default:
            break;
        }
        o.type_ = value_t::null;
    }

    void serialize(std::ostream& os) const {
        switch (type_) {
        case value_t::null:
            os << "null";
            break;
        case value_t::boolean:
            os << (b_ ? "true" : "false");
            break;
        case value_t::number_integer:
            os << i_;
            break;
        case value_t::number_unsigned:
            os << u_;
            break;
        case value_t::number_float:
            os << d_;
            break;
        case value_t::string: {
            os << '"';
            for (char c : s_) {
                switch (c) {
                case '"':  os << "\\\""; break;
                case '\\': os << "\\\\"; break;
                case '\b': os << "\\b";  break;
                case '\f': os << "\\f";  break;
                case '\n': os << "\\n";  break;
                case '\r': os << "\\r";  break;
                case '\t': os << "\\t";  break;
                default:   os << c;      break;
                }
            }
            os << '"';
            break;
        }
        case value_t::array:
            os << '[';
            if (arr_) {
                for (size_t i = 0; i < arr_->size(); ++i) {
                    if (i) os << ',';
                    (*arr_)[i].serialize(os);
                }
            }
            os << ']';
            break;
        case value_t::object:
            os << '{';
            if (obj_) {
                for (size_t i = 0; i < obj_->size(); ++i) {
                    if (i) os << ',';
                    os << '"' << (*obj_)[i].first << "\":";
                    (*obj_)[i].second.serialize(os);
                }
            }
            os << '}';
            break;
        default:
            os << "null";
            break;
        }
    }

    const json& at_key(const std::string& key) const {
        static const json null_j;
        if (!is_object() || !obj_)
            return null_j;
        for (const auto& kv : *obj_)
            if (kv.first == key)
                return kv.second;
        return null_j;
    }

    json& at_key_mut(const std::string& key) {
        if (!is_object()) {
            clear();
            type_ = value_t::object;
            obj_ = new object_t();
        }
        if (!obj_)
            obj_ = new object_t();
        for (auto& kv : *obj_)
            if (kv.first == key)
                return kv.second;
        obj_->emplace_back(key, json{});
        return obj_->back().second;
    }

    // ── Recursive-descent parser ─────────────────────────────────────────────
    struct Parser {
        const char* pos;
        const char* end;

        Parser(const char* p, const char* e) : pos(p), end(e) {}

        void skip_ws() {
            while (pos < end && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r'))
                ++pos;
        }

        char peek() {
            skip_ws();
            return (pos < end) ? *pos : '\0';
        }
        char consume() { return (pos < end) ? *pos++ : '\0'; }

        json parse_value() {
            char c = peek();
            if (c == '{')
                return parse_object();
            if (c == '[')
                return parse_array();
            if (c == '"')
                return json(parse_string());
            if (c == 't') {
                expect("true");
                json j(true);
                return j;
            }
            if (c == 'f') {
                expect("false");
                json j(false);
                return j;
            }
            if (c == 'n') {
                expect("null");
                return json();
            }
            if (c == '-' || (c >= '0' && c <= '9'))
                return parse_number();
            throw std::invalid_argument(std::string("unexpected char: ") + c);
        }

        json parse_object() {
            consume(); // '{'
            json j = json::object();
            j.obj_ = new object_t();
            skip_ws();
            if (peek() == '}') {
                consume();
                return j;
            }
            while (true) {
                skip_ws();
                if (peek() != '"')
                    throw std::invalid_argument("expected key");
                std::string key = parse_string();
                skip_ws();
                if (consume() != ':')
                    throw std::invalid_argument("expected ':'");
                json val = parse_value();
                j.obj_->emplace_back(std::move(key), std::move(val));
                skip_ws();
                char sep = consume();
                if (sep == '}')
                    break;
                if (sep != ',')
                    throw std::invalid_argument("expected ',' or '}'");
            }
            return j;
        }

        json parse_array() {
            consume(); // '['
            json j = json::array();
            j.arr_ = new array_t();
            skip_ws();
            if (peek() == ']') {
                consume();
                return j;
            }
            while (true) {
                j.arr_->push_back(parse_value());
                skip_ws();
                char sep = consume();
                if (sep == ']')
                    break;
                if (sep != ',')
                    throw std::invalid_argument("expected ',' or ']'");
            }
            return j;
        }

        std::string parse_string() {
            consume(); // '"'
            std::string s;
            while (pos < end) {
                char c = *pos++;
                if (c == '"')
                    return s;
                if (c == '\\') {
                    if (pos >= end)
                        break;
                    char esc = *pos++;
                    switch (esc) {
                    case '"':
                        s += '"';
                        break;
                    case '\\':
                        s += '\\';
                        break;
                    case '/':
                        s += '/';
                        break;
                    case 'b':
                        s += '\b';
                        break;
                    case 'f':
                        s += '\f';
                        break;
                    case 'n':
                        s += '\n';
                        break;
                    case 'r':
                        s += '\r';
                        break;
                    case 't':
                        s += '\t';
                        break;
                    case 'u': {
                        // 4-hex unicode: decode BMP codepoint to UTF-8
                        if (pos + 4 > end)
                            throw std::invalid_argument("short \\uXXXX");
                        uint32_t cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            char h = *pos++;
                            uint32_t nib;
                            if (h >= '0' && h <= '9')
                                nib = h - '0';
                            else if (h >= 'a' && h <= 'f')
                                nib = 10 + h - 'a';
                            else if (h >= 'A' && h <= 'F')
                                nib = 10 + h - 'A';
                            else
                                throw std::invalid_argument("bad hex");
                            cp = (cp << 4) | nib;
                        }
                        if (cp < 0x80) {
                            s += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            s += static_cast<char>(0xC0 | (cp >> 6));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            s += static_cast<char>(0xE0 | (cp >> 12));
                            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            s += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        s += esc;
                        break;
                    }
                } else {
                    s += c;
                }
            }
            throw std::invalid_argument("unterminated string");
        }

        json parse_number() {
            const char* start = pos;
            bool neg = false;
            if (*pos == '-') {
                neg = true;
                ++pos;
            }
            bool is_float = false;
            while (pos < end && *pos >= '0' && *pos <= '9')
                ++pos;
            if (pos < end && *pos == '.') {
                is_float = true;
                ++pos;
                while (pos < end && *pos >= '0' && *pos <= '9')
                    ++pos;
            }
            if (pos < end && (*pos == 'e' || *pos == 'E')) {
                is_float = true;
                ++pos;
                if (pos < end && (*pos == '+' || *pos == '-'))
                    ++pos;
                while (pos < end && *pos >= '0' && *pos <= '9')
                    ++pos;
            }
            std::string tok(start, pos - start);
            if (is_float) {
                return json(std::stod(tok));
            } else if (neg) {
                return json(static_cast<int64_t>(std::stoll(tok)));
            } else {
                uint64_t v = std::stoull(tok);
                return json(v);
            }
        }

        void expect(const char* str) {
            size_t len = std::strlen(str);
            if (static_cast<size_t>(end - pos) < len || std::strncmp(pos, str, len) != 0)
                throw std::invalid_argument(std::string("expected ") + str);
            pos += len;
        }
    };
};

template <> inline std::string json::get<std::string>() const {
    if (is_string())
        return s_;
    throw std::bad_cast();
}
template <> inline bool json::get<bool>() const {
    if (is_boolean())
        return b_;
    throw std::bad_cast();
}
template <> inline int64_t json::get<int64_t>() const {
    if (is_number_integer())
        return i_;
    if (is_number_unsigned())
        return static_cast<int64_t>(u_);
    if (is_number_float())
        return static_cast<int64_t>(d_);
    throw std::bad_cast();
}
template <> inline uint64_t json::get<uint64_t>() const {
    if (is_number_unsigned())
        return u_;
    if (is_number_integer())
        return static_cast<uint64_t>(i_);
    if (is_number_float())
        return static_cast<uint64_t>(d_);
    throw std::bad_cast();
}
template <> inline double json::get<double>() const {
    if (is_number_float())
        return d_;
    if (is_number_unsigned())
        return static_cast<double>(u_);
    if (is_number_integer())
        return static_cast<double>(i_);
    throw std::bad_cast();
}
template <> inline float json::get<float>() const { return static_cast<float>(get<double>()); }
template <> inline int json::get<int>() const { return static_cast<int>(get<int64_t>()); }

} // namespace nlohmann
