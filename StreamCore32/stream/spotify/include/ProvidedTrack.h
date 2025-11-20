
#include <cstring>
#include <utility>
#include <vector>
#include <string>
#include <stdlib.h>
#include <string.h>

#include "protobuf/player.pb.h"  // for PutStateRequest, DeviceState, PlayerState...
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json

#ifndef PB_STRDUP
#define PB_STRDUP strdup   // falls Nanopb's PB_STRDUP nicht verfügbar ist
#endif
#include <nlohmann/json.hpp>
using nlohmann::json;

static inline std::string json_to_meta_string(const nlohmann::json j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    // Für Zahlen, Arrays, Objekte, null: kompakte JSON-Repräsentation
    return j.dump(); 
}
bool add_metadata(player_proto_connect_ProvidedTrack *t, const char *k, const char *v) {
    size_t n = t->metadata_count + 1;
    // vergrößere das Array um 1
    void *p = realloc(t->metadata, n * sizeof *t->metadata);
    if (!p) return false;
    t->metadata = (MetadataEntry*)p;

    // neuen Slot initialisieren
    t->metadata[t->metadata_count] = (MetadataEntry)MetadataEntry_init_default;
    t->metadata[t->metadata_count].key   = strdup(k);  // oder strdup(k)
    t->metadata[t->metadata_count].value = strdup(v);
    t->full_metadata_count = n;
    t->metadata_count = n;
    return true;
}

bool add_metadata_list(player_proto_connect_ProvidedTrack *t,
                       const MetadataEntry *src,
                       size_t src_count)
{
    if (!t || !src || src_count == 0) return true;

    size_t old = t->metadata_count;
    size_t new_count = old + src_count;

    void *p = realloc(t->metadata, new_count * sizeof *t->metadata);
    if (!p) return false;
    t->metadata = (MetadataEntry*)p;

    // neue Slots initialisieren & deep copy
    size_t added = 0;
    for (size_t j = 0; j < src_count; ++j) {
        size_t dst = old + j;
        t->metadata[dst] =
            (MetadataEntry)MetadataEntry_init_default;

        const char *k = src[j].key   ? src[j].key   : "";
        const char *v = src[j].value ? src[j].value : "";

        t->metadata[dst].key   = PB_STRDUP(k);
        if (!t->metadata[dst].key) goto fail;

        t->metadata[dst].value = PB_STRDUP(v);
        if (!t->metadata[dst].value) goto fail;

        ++added;
    }
    t->full_metadata_count = new_count;
    t->metadata_count = new_count;
    return true;

fail:
    // Aufräumen der in diesem Batch allokierten Strings
    for (size_t j = 0; j < added; ++j) {
        size_t dst = old + j;
        free(t->metadata[dst].key);
        free(t->metadata[dst].value);
        t->metadata[dst] =
            (MetadataEntry)MetadataEntry_init_default;
    }
    // Count zurückrollen
    t->full_metadata_count = old;
    t->metadata_count = old;

    // Optional: Array wieder verkleinern (Fehlschlag ignorieren)
    void *q = realloc(t->metadata, old * sizeof *t->metadata);
    if (q) t->metadata = (MetadataEntry*)q;

    return false;
}

bool add_metadata_list(player_proto_connect_ProvidedTrack* t,
                       const std::vector<std::pair<std::string, std::string>>& metaVec)
{
    if (!t || metaVec.empty()) return true;

    const size_t old = t->metadata_count;
    const size_t add = metaVec.size();
    const size_t new_count = old + add;

    // Array auf neue Größe bringen
    void* p = std::realloc(t->metadata, new_count * sizeof *t->metadata);
    if (!p) return false;
    t->metadata = static_cast<MetadataEntry*>(p);

    // neue Slots initialisieren & deep-copy füllen
    size_t added = 0;
    for (size_t j = 0; j < add; ++j) {
        const size_t dst = old + j;
        t->metadata[dst] =
            (MetadataEntry)MetadataEntry_init_default;

        const char* k = metaVec[j].first.c_str();
        const char* v = metaVec[j].second.c_str();

        t->metadata[dst].key   = PB_STRDUP(k ? k : "");
        if (!t->metadata[dst].key) goto fail;

        t->metadata[dst].value = PB_STRDUP(v ? v : "");
        if (!t->metadata[dst].value) goto fail;

        ++added;
    }
    t->full_metadata_count = new_count;
    t->metadata_count = new_count;
    return true;

fail:
    // Alles aus diesem Batch wieder säubern
    for (size_t j = 0; j < added; ++j) {
        const size_t dst = old + j;
        std::free(t->metadata[dst].key);
        std::free(t->metadata[dst].value);
        t->metadata[dst] =
            (MetadataEntry)MetadataEntry_init_default;
    }
    // Count zurückrollen
    t->metadata_count = old;

    // Optional: wieder auf alte Größe schrumpfen (Fehlschlag ignorieren)
    if (old == 0) {
        std::free(t->metadata);
        t->metadata = nullptr;
    } else {
        void* q = std::realloc(t->metadata, old * sizeof *t->metadata);
        if (q) t->metadata = static_cast<MetadataEntry*>(q);
    }
    return false;
}
bool add_metadata_from_json_object(player_proto_connect_ProvidedTrack* t, const nlohmann::json& obj)
{
    if (!t) return false;
    if (!obj.is_object()) return true; // nichts zu tun

    const size_t add = obj.size();
    if (add == 0) return true;

    const size_t old = t->metadata_count;
    const size_t new_count = old + add;

    void* p = std::realloc(t->metadata, new_count * sizeof *t->metadata);
    if (!p) return false;
    t->metadata = static_cast<MetadataEntry*>(p);

    size_t added = 0;
    for (auto&& item : obj.items()) {
        const std::string& k = item.key();
        const std::string  v = json_to_meta_string(item.value());
        auto& dst = t->metadata[old + added];
        dst = (MetadataEntry)MetadataEntry_init_default;

        dst.key   = PB_STRDUP(k.c_str());
        if (!dst.key) goto fail;

        dst.value = PB_STRDUP(v.c_str());
        if (!dst.value) goto fail;

        ++added;
    }
    t->full_metadata_count = new_count;
    t->metadata_count = new_count;
    return true;

fail:
    // nur die in diesem Batch erzeugten Strings freigeben
    for (size_t j = 0; j < added; ++j) {
        auto& e = t->metadata[old + j];
        std::free(e.key);
        std::free(e.value);
        e = (MetadataEntry)MetadataEntry_init_default;
    }
    // optional wieder auf alte Größe schrumpfen
    if (old == 0) {
        std::free(t->metadata);
        t->metadata = nullptr;
    } else {
        void* q = std::realloc(t->metadata, old * sizeof *t->metadata);
        if (q) t->metadata = static_cast<MetadataEntry*>(q);
    }
    return false;
}