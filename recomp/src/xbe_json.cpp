// qilin info's JSON form of a loaded XBE (a tool, so std::format is fine
// here; the loader itself is runtime code, see strf.hpp).
#include "json.hpp"
#include "strf.hpp"
#include "xbe.hpp"

namespace qilin {

std::string Xbe::to_json() const {
    JsonWriter j(true);
    j.begin_object();
    j.kv("kind", kind);
    j.key("base").hex32(base);
    j.key("entry").hex32(entry);
    j.key("kernel_thunk").hex32(kthunk);
    j.kv("size_image", size_image);
    j.kv("stack_commit", stack_commit);
    j.kv("heap_reserve", heap_reserve);
    j.kv("heap_commit", heap_commit);
    j.key("init_flags").hex32(init_flags);
    j.key("tls_addr").hex32(tls_addr);
    j.kv("timestamp", timestamp);
    j.key("debug_path");
    if (debug_path.empty()) j.null(); else j.value(debug_path);
    j.key("certificate").begin_object();
    j.key("title_id").hex32(cert.title_id);
    j.kv("title_id_str", strf("%c%c-%03u", char(cert.title_id >> 24), char((cert.title_id >> 16) & 0xFF),
                                     cert.title_id & 0xFFFF));
    j.kv("title_name", cert.title_name);
    j.key("allowed_media").hex32(cert.allowed_media);
    j.key("region").hex32(cert.region);
    j.kv("ratings", cert.ratings);
    j.kv("disk_number", cert.disk_number);
    j.kv("version", cert.version);
    j.end_object();
    j.key("sections").begin_array();
    for (auto& s : sections) {
        j.begin_object();
        j.kv("name", s.name);
        j.key("flags").hex32(s.flags);
        j.key("va").hex32(s.va);
        j.kv("vsize", s.vsize);
        j.key("raw").hex32(s.raw);
        j.kv("rsize", s.rsize);
        j.kv("writable", s.writable());
        j.kv("preload", s.preload());
        j.kv("executable", s.executable());
        j.kv("inserted", s.inserted());
        j.end_object();
    }
    j.end_array();
    j.key("libraries").begin_array();
    for (auto& l : libraries) {
        j.begin_object();
        j.kv("name", l.name);
        j.kv("version", strf("%u.%u.%u", unsigned(l.major), unsigned(l.minor), unsigned(l.build)));
        j.kv("qfe", l.qfe);
        j.kv("approved", l.approved);
        j.kv("debug", l.debug);
        j.end_object();
    }
    j.end_array();
    j.key("kernel_imports").begin_array();
    for (auto o : kernel_imports) j.value(o);
    j.end_array();
    j.end_object();
    return j.str();
}

}  // namespace qilin
