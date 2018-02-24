#include "./message.h"

#include <sstream>

#include "./api.h"

using namespace std;
using boost::algorithm::replace_all;

namespace cq::message {
    string escape(string str) {
        replace_all(str, "&", "&amp;");
        replace_all(str, "[", "&#91;");
        replace_all(str, "]", "&#93;");
        replace_all(str, ",", "&#44;");
        return str;
    }

    string unescape(string str) {
        replace_all(str, "&#91;", "[");
        replace_all(str, "&#93;", "]");
        replace_all(str, "&#44;", ",");
        replace_all(str, "&amp;", "&");
        return str;
    }

    Message::Message(const string &msg_str) {
        // implement a DFA manually, because the regex lib of VC++ will throw stack overflow in some cases

        const static auto TEXT = 0;
        const static auto FUNCTION_NAME = 1;
        const static auto PARAMS = 2;
        auto state = TEXT;
        const auto end = msg_str.cend();
        stringstream text_s, function_name_s, params_s;
        auto curr_cq_start = end;
        for (auto it = msg_str.cbegin(); it != end; ++it) {
            const auto curr = *it;
            switch (state) {
            case TEXT: {
            text:
                if (curr == '[' && end - it >= 5 /* [CQ:a] at least 5 chars behind */
                    && *(it + 1) == 'C' && *(it + 2) == 'Q' && *(it + 3) == ':') {
                    state = FUNCTION_NAME;
                    curr_cq_start = it;
                    it += 3;
                } else {
                    text_s << curr;
                }
                break;
            }
            case FUNCTION_NAME: {
                if (curr >= 'A' && curr <= 'Z'
                    || curr >= 'a' && curr <= 'z'
                    || curr >= '0' && curr <= '9') {
                    function_name_s << curr;
                } else if (curr == ',') {
                    // function name out, params in
                    state = PARAMS;
                } else if (curr == ']') {
                    // CQ code end, with no params
                    goto params;
                } else {
                    // unrecognized character
                    text_s << string(curr_cq_start, it); // mark as text
                    curr_cq_start = end;
                    function_name_s = stringstream();
                    params_s = stringstream();
                    state = TEXT;
                    // because the current char may be '[', so we goto text part
                    goto text;
                }
                break;
            }
            case PARAMS: {
            params:
                if (curr == ']') {
                    // CQ code end
                    Segment seg;

                    seg.type = function_name_s.str();
                    while (params_s.rdbuf()->in_avail()) {
                        // split key and value
                        string key, value;
                        getline(params_s, key, '=');
                        getline(params_s, value, ',');
                        seg.data[key] = unescape(value);
                    }

                    if (text_s.rdbuf()->in_avail()) {
                        // there is a text segment before this CQ code
                        this->push_back(Segment{"text", {{"text", unescape(text_s.str())}}});
                        text_s = stringstream();
                    }

                    this->push_back(seg);
                    curr_cq_start = end;
                    text_s = stringstream();
                    function_name_s = stringstream();
                    params_s = stringstream();
                    state = TEXT;
                } else {
                    params_s << curr;
                }
            }
            default: break;
            }
        }

        // iterator end, there may be some rest of message we haven't put into segments
        switch (state) {
        case FUNCTION_NAME:
        case PARAMS:
            // we are in CQ code, but it ended with no ']', so it's a text segment
            text_s << string(curr_cq_start, end);
            // should fall through
        case TEXT:
            if (text_s.rdbuf()->in_avail()) {
                this->push_back(Segment{"text", {{"text", unescape(text_s.str())}}});
            }
        default: break;
        }
    }

    Message::operator string() const {
        stringstream ss;
        for (auto seg : *this) {
            if (seg.type.empty()) {
                continue;
            }
            if (seg.type == "text") {
                if (const auto it = seg.data.find("text"); it != seg.data.end()) {
                    ss << escape((*it).second);
                }
            } else {
                ss << "[CQ:" << seg.type;
                for (const auto item : seg.data) {
                    ss << "," << item.first << "=" << escape(item.second);
                }
                ss << "]";
            }
        }
        return ss.str();
    }

    int32_t Message::send(const Target &target) const {
        return api::send_msg(target, *this);
    }

    void Message::reduce() {
        if (this->empty()) {
            return;
        }

        auto last_seg_it = this->begin();
        for (auto it = this->begin(); ++it != this->end();) {
            if (it->type == "text" && last_seg_it->type == "text"
                && it->data.find("text") != it->data.end()
                && last_seg_it->data.find("text") != last_seg_it->data.end()) {
                // found adjacent "text" segments
                last_seg_it->data["text"] += it->data["text"];
                // remove the current element and continue
                this->erase(it);
                it = last_seg_it;
            } else {
                last_seg_it = it;
            }
        }
    }
}
