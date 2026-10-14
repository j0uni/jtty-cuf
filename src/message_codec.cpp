#include "jtty/message_codec.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <tuple>

namespace jtty {
namespace {

constexpr std::string_view alphabet =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ +-./?!\"#$%,&*()_'=[]{}<>|:;";
constexpr std::string_view base36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view call_first = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view call_second = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::string_view call_digit = "0123456789";
constexpr std::string_view call_suffix = " ABCDEFGHIJKLMNOPQRSTUVWXYZ";
constexpr std::array<std::uint8_t, 13> sync_tones = {0, 2, 2, 3, 0, 0, 3, 2, 1, 3, 1, 2, 0};
constexpr std::uint32_t call_offset = 6'257'896;
constexpr std::array<std::string_view, 18> controls = {
    "AGN?", "CALL?", "AGN CALL", "NR?", "AGN NR", "EXCH?", "STATE?", "SECTION?",
    "ZONE?", "GRID?", "RPRT?", "QSL TU", "TU", "QRZ?", "QSO B4", "WAIT", "NIL?", "OK?"};
constexpr std::array<std::string_view, 86> sections = {
    "AB","AK","AL","AR","AZ","BC","CO","CT","DE","EB","EMA","ENY","EPA","EWA","GA","GH","IA","ID","IL","IN",
    "KS","KY","LA","LAX","NS","MB","MDC","ME","MI","MN","MO","MS","MT","NC","ND","NE","NFL","NH","NL","NLI",
    "NM","NNJ","NNY","TER","NTX","NV","OH","OK","ONE","ONN","ONS","OR","ORG","PAC","PR","QC","RI","SB","SC","SCV",
    "SD","SDG","SF","SFL","SJV","SK","SNJ","STX","SV","TN","UT","VA","VI","VT","WCF","WI","WMA","WNY","WPA","WTX",
    "WV","WWA","WY","DX","PE","NB"};

struct AtomWord {
    std::uint32_t word{};
    std::string atom;
    std::string subtype;
    std::string rendering;
    int family_rank{};
    int subtype_rank{};
    bool full_role{};
};

unsigned parity(unsigned value) { return std::popcount(value) & 1U; }
std::uint16_t crc_step(std::uint16_t reg, std::uint8_t bit) {
    reg ^= static_cast<std::uint16_t>(bit) << 11U;
    reg = (reg & 0x800U) != 0U ? static_cast<std::uint16_t>((reg << 1U) ^ 0x80FU)
                              : static_cast<std::uint16_t>(reg << 1U);
    return reg & 0xFFFU;
}
std::uint16_t next_state(std::uint16_t state, std::uint8_t bit) {
    return static_cast<std::uint16_t>(((state << 1U) | bit) & 0x1FFU);
}
std::array<std::uint8_t, 2> code_output(std::uint16_t state, std::uint8_t bit) {
    const unsigned reg = (static_cast<unsigned>(state) << 1U) | bit;
    return {static_cast<std::uint8_t>(parity(reg & 01167U)),
            static_cast<std::uint8_t>(parity(reg & 01545U))};
}

std::string format_decimal(std::uint32_t value, unsigned width = 0) {
    std::ostringstream out;
    out << std::setw(static_cast<int>(width)) << std::setfill('0') << value;
    return out.str();
}

std::optional<std::uint32_t> decimal(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return std::nullopt;
    std::uint32_t result = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') return std::nullopt;
        if (result > (std::numeric_limits<std::uint32_t>::max() - unsigned(ch - '0')) / 10U) return std::nullopt;
        result = result * 10U + unsigned(ch - '0');
    }
    return result;
}

std::optional<std::uint32_t> base36_value(std::string_view token) {
    if (token.size() < 2 || token.size() > 3) return std::nullopt;
    std::uint32_t value = 0;
    for (char ch : token) {
        const auto at = base36.find(ch);
        if (at == std::string_view::npos) return std::nullopt;
        value = value * 36U + static_cast<std::uint32_t>(at);
    }
    if (token.size() == 2 && value >= 36U * 36U) return std::nullopt;
    if (token.size() == 3 && value < 36U * 36U) return std::nullopt;
    return value;
}

std::string base36_text(std::uint32_t value, unsigned length) {
    std::string result(length, '0');
    for (unsigned i = 0; i < length; ++i) {
        result[length - 1U - i] = base36[value % 36U];
        value /= 36U;
    }
    return result;
}

std::optional<std::size_t> call_area_digit(std::string_view call) {
    if (call.size() < 3 || call.size() > 6) return std::nullopt;
    for (const std::size_t digit : {std::size_t{1}, std::size_t{2}}) {
        if (digit >= call.size() || !std::isdigit(static_cast<unsigned char>(call[digit])) ||
            digit + 1 >= call.size() || call.size() - digit - 1 > 3) continue;
        bool prefix_letter = false;
        bool prefix_valid = true;
        for (std::size_t i = 0; i < digit; ++i) {
            prefix_valid &= std::isalnum(static_cast<unsigned char>(call[i])) != 0;
            prefix_letter |= call[i] >= 'A' && call[i] <= 'Z';
        }
        const bool suffix_valid = std::all_of(
            call.begin() + static_cast<std::ptrdiff_t>(digit + 1), call.end(),
            [](char ch) { return ch >= 'A' && ch <= 'Z'; });
        if (prefix_valid && prefix_letter && suffix_valid) return digit;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> encode_call(std::string_view call) {
    const auto digit = call_area_digit(call);
    if (!digit) return std::nullopt;
    std::string aligned(call);
    if (*digit == 1) aligned.insert(aligned.begin(), ' ');
    aligned.resize(6, ' ');
    const std::array<std::string_view, 6> alphabets = {call_first, call_second, call_digit,
                                                       call_suffix, call_suffix, call_suffix};
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        const auto at = alphabets[i].find(aligned[i]);
        if (at == std::string_view::npos) return std::nullopt;
        value = value * static_cast<std::uint32_t>(alphabets[i].size()) + static_cast<std::uint32_t>(at);
    }
    return value + call_offset;
}

std::optional<std::string> decode_call(std::uint32_t encoded) {
    if (encoded < call_offset) return std::nullopt;
    std::uint32_t value = encoded - call_offset;
    constexpr std::array<unsigned, 6> radices = {37, 36, 10, 27, 27, 27};
    std::array<unsigned, 6> digits{};
    for (int i = 5; i >= 0; --i) { digits[static_cast<unsigned>(i)] = value % radices[static_cast<unsigned>(i)]; value /= radices[static_cast<unsigned>(i)]; }
    if (value != 0) return std::nullopt;
    std::string aligned;
    aligned.push_back(call_first[digits[0]]); aligned.push_back(call_second[digits[1]]);
    aligned.push_back(call_digit[digits[2]]);
    aligned.push_back(call_suffix[digits[3]]); aligned.push_back(call_suffix[digits[4]]); aligned.push_back(call_suffix[digits[5]]);
    while (!aligned.empty() && aligned.back() == ' ') aligned.pop_back();
    if (!aligned.empty() && aligned.front() == ' ') aligned.erase(aligned.begin());
    const auto roundtrip = encode_call(aligned);
    if (!roundtrip || *roundtrip != encoded) return std::nullopt;
    return aligned;
}

Result<AtomWord> call_atom(std::string_view subtype, std::string_view call) {
    constexpr std::array<std::string_view, 6> names = {"cq","call","tu_cq","call_tu","call_agn","tu_now"};
    constexpr std::array<std::string_view, 6> renders = {"CQ %s CQ","%s","TU %s CQ","%s TU","%s AGN?","TU NOW %s"};
    const auto it = std::find(names.begin(), names.end(), subtype);
    if (it == names.end()) return {.error = "invalid call action"};
    const auto encoded = encode_call(call);
    if (!encoded) return {.error = "invalid call"};
    const unsigned action = static_cast<unsigned>(it - names.begin());
    std::string rendering(renders[action]);
    const auto marker = rendering.find("%s"); rendering.replace(marker, 2, call);
    return {.value = {(*encoded << 4U) | ((action & 3U) << 2U) | (action >= 4U ? 1U : 0U),
                      "CALL28", std::string(subtype), std::move(rendering), 0, static_cast<int>(action), false}};
}

Result<AtomWord> number_atom(std::string_view kind, bool full, std::uint32_t value) {
    constexpr std::array<std::string_view, 8> names = {"serial","cq_zone","itu_zone","age","power","check","first_license_year","generic_numeric"};
    const auto it = std::find(names.begin(), names.end(), kind);
    if (it == names.end()) return {.error = "invalid numeric kind"};
    const unsigned index = static_cast<unsigned>(it - names.begin());
    if (value > 131071U || (index == 1 && (value < 1 || value > 40)) ||
        (index == 2 && (value < 1 || value > 90)) || (index == 6 && value > 9999)) return {.error = "numeric field out of range"};
    const unsigned width = index == 0 ? 3 : (index == 1 || index == 2 || index == 5 ? 2 : (index == 6 ? 4 : 0));
    std::string rendering = format_decimal(value, width);
    if (full) rendering = "599 " + rendering;
    const std::uint32_t body = (std::uint32_t(full) << 26U) | (index << 22U) | (value << 5U);
    return {.value = {(body << 5U) | 2U, "EXCH_NUM", std::string(kind), std::move(rendering), 1, static_cast<int>(index), full}};
}

Result<AtomWord> location_atom(std::string_view kind, bool full, std::string_view token) {
    constexpr std::array<std::string_view, 5> names = {"state_province","arrl_rac_section","country_prefix","generic_qth","local_admin"};
    const auto it = std::find(names.begin(), names.end(), kind);
    if (it == names.end()) return {.error = "invalid location kind"};
    const auto value = base36_value(token);
    if (!value) return {.error = "noncanonical location token"};
    const unsigned index = static_cast<unsigned>(it - names.begin());
    const std::uint32_t body = (std::uint32_t(full) << 26U) | (index << 22U) |
                               (std::uint32_t(token.size() == 3) << 21U) | (*value << 5U);
    std::string rendering(token); if (full) rendering = "599 " + rendering;
    return {.value = {(body << 5U) | (1U << 2U) | 2U, "EXCH_LOC", std::string(kind), std::move(rendering), 2, static_cast<int>(index), full}};
}

Result<AtomWord> zone_atom(std::uint32_t zone, std::string_view token) {
    const auto value = base36_value(token);
    if (zone < 1 || zone > 40 || !value) return {.error = "invalid zone/location pair"};
    const std::uint32_t body = (zone << 18U) | (std::uint32_t(token.size() == 3) << 17U) | (*value << 1U);
    return {.value = {(body << 5U) | (2U << 2U) | 2U, "EXCH_PAIR", "zone_loc3",
                      "599 " + format_decimal(zone, 2) + " " + std::string(token), 3, 0, true}};
}

Result<AtomWord> class_atom(std::uint32_t count, char klass, std::string_view section) {
    const auto found = std::find(sections.begin(), sections.end(), section);
    if (count < 1 || count > 32 || klass < 'A' || klass > 'F' || found == sections.end()) return {.error = "invalid class/section"};
    const unsigned section_id = static_cast<unsigned>(found - sections.begin()) + 1U;
    const std::uint32_t body = (1U << 24U) | (count << 18U) | (std::uint32_t(klass - 'A') << 15U) | (section_id << 8U);
    return {.value = {(body << 5U) | (2U << 2U) | 2U, "EXCH_PAIR", "class_section",
                      std::to_string(count) + klass + " " + std::string(section), 3, 1, false}};
}

Result<AtomWord> time_atom(bool full, std::uint32_t serial, std::uint32_t minute) {
    if (serial > 16383 || minute > 1439) return {.error = "serial/time field out of range"};
    const std::uint32_t body = (std::uint32_t(full) << 26U) | (serial << 12U) | (minute << 1U);
    const auto hhmm = format_decimal(minute / 60U, 2) + format_decimal(minute % 60U, 2);
    std::string rendering = format_decimal(serial, 3) + " " + hhmm; if (full) rendering = "599 " + rendering;
    return {.value = {(body << 5U) | (3U << 2U) | 2U, "EXCH_NUM_TIME", "serial_utc", std::move(rendering), 4, 0, full}};
}

Result<AtomWord> control_atom(std::string_view phrase) {
    const auto found = std::find(controls.begin(), controls.end(), phrase);
    if (found == controls.end()) return {.error = "unregistered control phrase"};
    const unsigned id = static_cast<unsigned>(found - controls.begin());
    constexpr std::array<std::string_view,18> names = {"agn","call","agn_call","nr","agn_nr","exch","state","section","zone","grid","rprt","qsl_tu","tu","qrz","qso_b4","wait","nil","ok"};
    const std::uint32_t body = id << 16U;
    return {.value = {(body << 5U) | (4U << 2U) | 2U, "CONTROL", std::string(names[id]), std::string(phrase), 5, static_cast<int>(id), false}};
}

Result<AtomWord> grid_atom(bool full, std::string_view grid) {
    if (grid.size() != 4 || grid[0] < 'A' || grid[0] > 'R' || grid[1] < 'A' || grid[1] > 'R' ||
        grid[2] < '0' || grid[2] > '9' || grid[3] < '0' || grid[3] > '9') return {.error = "invalid grid"};
    const std::uint32_t code = (((std::uint32_t(grid[0]-'A') * 18U + std::uint32_t(grid[1]-'A')) * 10U + std::uint32_t(grid[2]-'0')) * 10U + std::uint32_t(grid[3]-'0'));
    const std::uint32_t body = (1U << 23U) | (std::uint32_t(full) << 22U) | (code << 7U);
    std::string rendering(grid); if (full) rendering = "599 " + rendering;
    return {.value = {(body << 5U) | (4U << 2U) | 2U, "GRID4", "grid4", std::move(rendering), 6, 0, full}};
}

Result<AtomWord> text_atom(std::string_view text, bool final) {
    std::string canonical(text);
    if (final) while (!canonical.empty() && canonical.back() == ' ') canonical.pop_back();
    if (canonical.empty() || canonical.size() > 5 || (!final && canonical.size() != 5)) return {.error = "invalid TEXT5 length"};
    std::string padded(canonical); padded.resize(5, ' ');
    std::uint32_t body = 0;
    for (char ch : padded) { const auto at = alphabet.find(ch); if (at == std::string_view::npos) return {.error = "unsupported TEXT5 character"}; body = (body << 6U) | static_cast<std::uint32_t>(at); }
    return {.value = {(body << 2U) | 3U, "TEXT5", "literal", std::move(canonical), 7, 0, false}};
}

Result<AtomWord> typed_atom(const MessageAtom& atom, bool final) {
    switch (atom.type) {
    case AtomType::call: return call_atom(atom.subtype, atom.text);
    case AtomType::number: return number_atom(atom.subtype, atom.full_role, atom.value);
    case AtomType::location: return location_atom(atom.subtype, atom.full_role, atom.text);
    case AtomType::zone_loc3: return zone_atom(atom.value, atom.text);
    case AtomType::class_section: return atom.text.size() < 2 ? Result<AtomWord>{.error="invalid class/section"} : class_atom(atom.value, atom.text.front(), std::string_view(atom.text).substr(1));
    case AtomType::number_time: return time_atom(atom.full_role, atom.value, atom.secondary_value);
    case AtomType::control: return control_atom(atom.text);
    case AtomType::grid4: return grid_atom(atom.full_role, atom.text);
    case AtomType::text5: return text_atom(atom.text, final);
    }
    return {.error = "invalid atom type"};
}

PayloadBits payload_from_word(std::uint32_t word, bool eom) {
    PayloadBits payload{};
    for (unsigned i = 0; i < 32; ++i) payload[i] = static_cast<std::uint8_t>((word >> (31U-i)) & 1U);
    payload[32] = 0; payload[33] = static_cast<std::uint8_t>(eom);
    return payload;
}

std::uint32_t word_from_payload(const PayloadBits& payload) {
    std::uint32_t word = 0; for (unsigned i=0;i<32;++i) word = (word<<1U)|payload[i]; return word;
}

Result<MessageFrame> make_frame(const AtomWord& atom, bool eom) {
    auto wire = encode_payload(payload_from_word(atom.word, eom));
    if (!wire) return {.error = wire.error};
    wire.value.canonical_text = atom.rendering;
    return {.value = {atom.atom, atom.subtype, atom.rendering, std::move(wire.value)}};
}

Result<AtomWord> decode_word(std::uint32_t word, bool final) {
    if (word == 0) return {.error = "all-zero grammar sentinel"};
    const unsigned top = word & 3U;
    if (top == 3U) {
        std::string text; const std::uint32_t chars = word >> 2U;
        for (int shift=24;shift>=0;shift-=6) text.push_back(alphabet[(chars >> unsigned(shift)) & 63U]);
        if (final) while (!text.empty() && text.back()==' ') text.pop_back();
        return text_atom(text, final);
    }
    if (top <= 1U) {
        const unsigned action = ((word >> 2U) & 3U) + (top ? 4U : 0U);
        if (action >= 6) return {.error = "unassigned call action"};
        const auto call = decode_call(word >> 4U); if (!call) return {.error = "invalid compact call"};
        constexpr std::array<std::string_view,6> names={"cq","call","tu_cq","call_tu","call_agn","tu_now"};
        return call_atom(names[action],*call);
    }
    const unsigned family = (word >> 2U) & 7U; const std::uint32_t body = word >> 5U;
    if (family == 0) {
        if ((body & 31U)!=0) return {.error="numeric reserved bits nonzero"};
        const bool role=(body>>26U)&1U; const unsigned kind=(body>>22U)&15U; const auto value=(body>>5U)&0x1FFFFU;
        constexpr std::array<std::string_view,8> names={"serial","cq_zone","itu_zone","age","power","check","first_license_year","generic_numeric"};
        if(kind>=names.size()) return {.error="reserved numeric kind"}; return number_atom(names[kind],role,value);
    }
    if (family == 1) {
        if ((body&31U)!=0) return {.error="location reserved bits nonzero"};
        const bool role=(body>>26U)&1U; const unsigned kind=(body>>22U)&15U; const unsigned len=((body>>21U)&1U)+2U; const auto value=(body>>5U)&0xFFFFU;
        constexpr std::array<std::string_view,5> names={"state_province","arrl_rac_section","country_prefix","generic_qth","local_admin"};
        if(kind>=names.size() || (len==2&&value>=1296) || (len==3&&(value<1296||value>=46656))) return {.error="invalid location field"};
        return location_atom(names[kind],role,base36_text(value,len));
    }
    if (family == 2) {
        if ((body&1U)!=0) return {.error="pair reserved bit nonzero"}; const unsigned schema=(body>>24U)&7U;
        if(schema==0){ const auto zone=(body>>18U)&63U; const unsigned len=((body>>17U)&1U)+2U; const auto value=(body>>1U)&0xFFFFU; if((len==2&&value>=1296)||(len==3&&(value<1296||value>=46656))) return {.error="invalid paired token"}; return zone_atom(zone,base36_text(value,len)); }
        if(schema==1){ if((body&0xFFU)!=0) return {.error="class reserved bits nonzero"}; const auto count=(body>>18U)&63U; const auto klass=(body>>15U)&7U; const auto section=(body>>8U)&127U; if(section<1||section>sections.size()) return {.error="invalid section"}; return class_atom(count,char('A'+klass),sections[section-1]); }
        return {.error="reserved pair schema"};
    }
    if (family == 3) { if((body&1U)!=0) return {.error="time reserved bit nonzero"}; return time_atom((body>>26U)&1U,(body>>12U)&0x3FFFU,(body>>1U)&0x7FFU); }
    if (family == 4) {
        const unsigned subtype=(body>>23U)&15U;
        if(subtype==0){ if((body&0xFFFFU)!=0) return {.error="control reserved bits nonzero"}; const unsigned id=(body>>16U)&127U; if(id>=controls.size()) return {.error="unassigned control"}; return control_atom(controls[id]); }
        if(subtype==1){ if((body&0x7FU)!=0) return {.error="grid reserved bits nonzero"}; const bool role=(body>>22U)&1U; auto code=(body>>7U)&0x7FFFU; if(code>=32400) return {.error="grid code out of range"}; std::string grid(4,'0'); grid[3]=char('0'+code%10);code/=10;grid[2]=char('0'+code%10);code/=10;grid[1]=char('A'+code%18);code/=18;grid[0]=char('A'+code); return grid_atom(role,grid); }
        return {.error="reserved MISC subtype"};
    }
    return {.error="reserved STRUCT30 family"};
}

std::string normalize_literal(std::string_view input) {
    input = input.substr(0, max_message_bytes);
    std::string output; output.reserve(input.size()); bool pending=false;
    for(unsigned char raw:input){ if(raw==' '){pending=!output.empty();continue;} char ch=static_cast<char>(raw); if(ch>='a'&&ch<='z')ch=char(ch-'a'+'A'); if(alphabet.find(ch)==std::string_view::npos)ch='#'; if(pending){output.push_back(' ');pending=false;} output.push_back(ch); }
    return output;
}

std::string canonicalize_rtty_serials(std::string text) {
    std::size_t at=0;
    while((at=text.find("599 ",at))!=std::string::npos){ if(at!=0&&text[at-1]!=' '){at+=4;continue;} const auto start=at+4; auto end=text.find(' ',start); if(end==std::string::npos)end=text.size(); const auto token=std::string_view(text).substr(start,end-start); bool digits=token.size()>=1&&token.size()<=6&&std::all_of(token.begin(),token.end(),[](char c){return c>='0'&&c<='9';}); if(digits){std::uint32_t value=0;for(char c:token)value=value*10U+unsigned(c-'0');if(value<=131071){const auto replacement=format_decimal(value,3);text.replace(start,token.size(),replacement);at=start+replacement.size();continue;}} at=start; }
    return text;
}

std::vector<AtomWord> structured_at(std::string_view source, std::size_t pos, MessageProfile profile) {
    std::vector<AtomWord> result;
    if (pos != 0 && source[pos - 1] != ' ') return result;
    const auto add=[&](Result<AtomWord> atom){if(atom&&source.substr(pos,atom.value.rendering.size())==atom.value.rendering){const auto end=pos+atom.value.rendering.size();if(end==source.size()||source[end]==' ')result.push_back(std::move(atom.value));}};
    // Calls and controls are a small closed vocabulary; discover the call token from each rendering form.
    const auto end_token=source.find(' ',pos); const auto first=source.substr(pos,(end_token==std::string_view::npos?source.size():end_token)-pos);
    add(call_atom("call",first)); add(call_atom("cq",source.substr(pos+std::min<std::size_t>(3,source.size()-pos),6)));
    for(std::size_t len=3;len<=6&&pos+len<=source.size();++len){const auto c=source.substr(pos,len);add(call_atom("call",c));add(call_atom("call_tu",c));add(call_atom("call_agn",c));}
    if(source.substr(pos).starts_with("TU ")) for(std::size_t len=3;len<=6&&pos+3+len<=source.size();++len){const auto c=source.substr(pos+3,len);add(call_atom("tu_cq",c));}
    if(source.substr(pos).starts_with("TU NOW ")) for(std::size_t len=3;len<=6&&pos+7+len<=source.size();++len)add(call_atom("tu_now",source.substr(pos+7,len)));
    if(source.substr(pos).starts_with("CQ ")) for(std::size_t len=3;len<=6&&pos+3+len<=source.size();++len)add(call_atom("cq",source.substr(pos+3,len)));
    for(auto phrase:controls)add(control_atom(phrase));
    // Interpret complete spans up to the production rendering bound.
    for(std::size_t length=1;length<=15&&pos+length<=source.size();++length){const auto span=source.substr(pos,length);if(pos+length<source.size()&&source[pos+length]!=' ')continue;
        bool full=span.starts_with("599 "); auto field=full?span.substr(4):span;
        if(auto value=decimal(field);value&&*value<=131071)add(number_atom("generic_numeric",full,*value));
        if(profile==MessageProfile::rtty_roundup&&full&&field.size()>=3&&field.size()<=6&&
           std::all_of(field.begin(),field.end(),[](char c){return c>='0'&&c<='9';})) {
            std::uint32_t value=0;for(char c:field)value=value*10U+unsigned(c-'0');
            if(value<=131071&&field==format_decimal(value,3))add(number_atom("serial",true,value));
        }
        if(full&&field.size()>=2&&field.size()<=3&&std::any_of(field.begin(),field.end(),[](char c){return c>='A'&&c<='Z';})){add(location_atom("generic_qth",true,field));if(profile==MessageProfile::rtty_roundup)add(location_atom("state_province",true,field));}
        add(grid_atom(full,field));
        const auto space=span.find(' ');if(space!=std::string_view::npos&&!full){const auto left=span.substr(0,space),right=span.substr(space+1);if(left.size()>=2&&left.back()>='A'&&left.back()<='F'){if(auto count=decimal(left.substr(0,left.size()-1)))add(class_atom(*count,left.back(),right));}}
    }
    std::sort(result.begin(),result.end(),[](const AtomWord&a,const AtomWord&b){return std::tie(a.word,a.atom,a.subtype)<std::tie(b.word,b.atom,b.subtype);});
    result.erase(std::unique(result.begin(),result.end(),[](const AtomWord&a,const AtomWord&b){return a.word==b.word;}),result.end()); return result;
}

struct Choice { bool reachable{}; unsigned frames=999; AtomWord atom; std::size_t next{}; std::size_t consumed{}; };
bool better(unsigned frames,const AtomWord&atom,std::size_t consumed,const Choice&old){if(!old.reachable||frames<old.frames)return true;if(frames>old.frames)return false;const bool structured=atom.atom!="TEXT5",old_structured=old.atom.atom!="TEXT5";if(structured!=old_structured)return structured;if(consumed!=old.consumed)return consumed>old.consumed;return std::tie(atom.family_rank,atom.subtype_rank,atom.full_role)<std::tie(old.atom.family_rank,old.atom.subtype_rank,old.atom.full_role);}

}  // namespace

Result<EncodedFrame> encode_payload(const PayloadBits& payload) {
    if(std::any_of(payload.begin(),payload.end(),[](auto bit){return bit>1;}))return {.error="payload bits must contain only 0 or 1"};
    EncodedFrame frame; frame.payload=payload; frame.message_word=word_from_payload(payload); frame.end_of_message=payload[33]!=0;
    std::copy(payload.begin(),payload.end(),frame.information.begin()); std::uint16_t remainder=0;for(auto bit:payload)remainder=crc_step(remainder,bit);
    for(unsigned i=0;i<12;++i){auto bit=std::uint8_t((remainder>>(11U-i))&1U);frame.crc[i]=bit;frame.information[34+i]=bit;}
    std::uint16_t state=0;for(unsigned i=37;i<46;++i)state=next_state(state,frame.information[i]);
    for(unsigned i=0;i<46;++i){auto out=code_output(state,frame.information[i]);frame.coded[2*i]=out[0];frame.coded[2*i+1]=out[1];state=next_state(state,frame.information[i]);}
    std::copy(sync_tones.begin(),sync_tones.end(),frame.tones.begin());constexpr std::array<std::uint8_t,4> map={0,1,3,2};for(unsigned i=0;i<46;++i)frame.tones[13+i]=map[(frame.coded[2*i]<<1U)|frame.coded[2*i+1]];
    return {.value=std::move(frame)};
}

Result<EncodedMessage> pack_typed_message(std::span<const MessageAtom> atoms) {
    if(atoms.empty())return {.error="empty message"};if(atoms.size()>max_message_frames)return {.error="too many frames"};
    std::vector<AtomWord> words;words.reserve(atoms.size());std::string canonical;
    for(std::size_t i=0;i<atoms.size();++i){auto atom=typed_atom(atoms[i],i+1==atoms.size());if(!atom)return {.error=atom.error};if(i&&words.back().atom!="TEXT5")canonical.push_back(' ');canonical+=atom.value.rendering;if(canonical.size()>max_message_bytes)return {.error="canonical message exceeds 80 bytes"};words.push_back(std::move(atom.value));}
    EncodedMessage message{canonical,{}};for(std::size_t i=0;i<words.size();++i){auto frame=make_frame(words[i],i+1==words.size());if(!frame)return {.error=frame.error};message.frames.push_back(std::move(frame.value));}return {.value=std::move(message)};
}

Result<EncodedMessage> pack_literal_message(std::string_view input, MessageProfile profile) {
    if(input.size()>4096)return {.error="literal input exceeds 4096-byte bound"};auto source=normalize_literal(input);if(source.empty())return {.error="empty message"};if(profile==MessageProfile::rtty_roundup)source=canonicalize_rtty_serials(std::move(source));if(source.size()>max_message_bytes)return {.error="canonical message exceeds 80 bytes"};
    std::vector<Choice> dp(source.size()+1);dp[source.size()]={true,0,{},{},0};
    for(std::size_t pos=source.size();pos-->0;){
        auto candidates=structured_at(source,pos,profile);const auto remaining=source.size()-pos;const auto text_len=std::min<std::size_t>(5,remaining);if(text_len==5||pos+text_len==source.size()){auto text=text_atom(std::string_view(source).substr(pos,text_len),pos+text_len==source.size());if(text)candidates.push_back(std::move(text.value));}
        for(auto&atom:candidates){std::size_t consumed=atom.rendering.size();if(atom.atom=="TEXT5")consumed=atom.rendering.size();else if(pos+consumed<source.size()&&source[pos+consumed]==' ')++consumed;if(pos+consumed>source.size()||!dp[pos+consumed].reachable)continue;const unsigned frames=1+dp[pos+consumed].frames;if(better(frames,atom,consumed,dp[pos]))dp[pos]={true,frames,atom,pos+consumed,consumed};}
    }
    if(!dp[0].reachable||dp[0].frames>max_message_frames)return {.error="message cannot be packed within 16 frames"};
    EncodedMessage message{source,{}};for(std::size_t pos=0;pos<source.size();pos=dp[pos].next){const bool eom=dp[pos].next==source.size();auto frame=make_frame(dp[pos].atom,eom);if(!frame)return {.error=frame.error};message.frames.push_back(std::move(frame.value));}return {.value=std::move(message)};
}

Result<DecodedFrameAtom> decode_frame_payload(const PayloadBits& payload) {
    if (std::any_of(payload.begin(), payload.end(), [](auto bit) { return bit > 1; })) return {.error = "invalid payload bit"};
    if (payload[32]) return {.error = "reserved payload bit is set"};
    const bool eom = payload[33] != 0;
    const std::uint32_t word = word_from_payload(payload);
    auto atom = decode_word(word, eom);
    if (!atom) return {.error = atom.error};
    DecodedFrameAtom out{atom.value.atom, atom.value.rendering, {}, eom};
    if (atom.value.atom == "TEXT5") {
        const std::uint32_t chars = word >> 2U;
        for (int shift = 24; shift >= 0; shift -= 6) out.text5_raw.push_back(alphabet[(chars >> unsigned(shift)) & 63U]);
    }
    return {.value = std::move(out)};
}

Result<EncodedMessage> decode_message_payloads(std::span<const PayloadBits> payloads) {
    if(payloads.empty()||payloads.size()>max_message_frames)return {.error="payload count outside 1..16"};EncodedMessage message;
    for(std::size_t i=0;i<payloads.size();++i){const auto&p=payloads[i];if(std::any_of(p.begin(),p.end(),[](auto bit){return bit>1;}))return {.error="invalid payload bit"};if(p[32])return {.error="reserved payload bit is set"};if(bool(p[33])!=(i+1==payloads.size()))return {.error="EOM must appear only on final payload"};auto atom=decode_word(word_from_payload(p),p[33]!=0);if(!atom)return {.error=atom.error};if(!message.frames.empty()&&message.frames.back().atom!="TEXT5")message.canonical_text.push_back(' ');message.canonical_text+=atom.value.rendering;if(message.canonical_text.size()>max_message_bytes)return {.error="decoded message exceeds 80 bytes"};auto frame=make_frame(atom.value,p[33]!=0);if(!frame)return {.error=frame.error};message.frames.push_back(std::move(frame.value));}
    return {.value=std::move(message)};
}

Result<std::vector<std::vector<float>>> modulate_message_frames(std::span<const MessageFrame> frames,double base,double amplitude){if(frames.empty()||frames.size()>max_message_frames)return {.error="frame count outside 1..16"};std::vector<std::vector<float>> result;result.reserve(frames.size());for(const auto&frame:frames){auto pcm=modulate(frame.wire.tones,base,amplitude);if(!pcm)return {.error=pcm.error};result.push_back(std::move(pcm.value));}return {.value=std::move(result)};}

Result<std::vector<float>> modulate_message(std::span<const MessageFrame> frames,
                                            double base, double amplitude) {
    if (frames.empty() || frames.size() > max_message_frames) {
        return {.error = "frame count outside 1..16"};
    }
    std::vector<ToneFrame> tones;
    tones.reserve(frames.size());
    for (const auto& frame : frames) tones.push_back(frame.wire.tones);
    return modulate_frames(tones, base, amplitude);
}

Result<std::vector<float>> modulate_message_48k(std::span<const MessageFrame> frames,
                                                double base, double amplitude) {
    if (frames.empty() || frames.size() > max_message_frames) {
        return {.error = "frame count outside 1..16"};
    }
    std::vector<ToneFrame> tones;
    tones.reserve(frames.size());
    for (const auto& frame : frames) tones.push_back(frame.wire.tones);
    return modulate_frames_48k(tones, base, amplitude);
}

}  // namespace jtty
