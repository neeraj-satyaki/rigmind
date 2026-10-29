#pragma once
// Per-frame EWS facts, carried on a GstBuffer as custom metadata.
// The upstream detector attaches them; rigmindews reads them. Geometry is decided upstream (see alarm_state.h).
#include <gst/gst.h>
#include <new>
#include "backend/alarm_state.h"

G_BEGIN_DECLS

typedef struct _RigmindFactsMeta {
    GstMeta meta;
    rigmind::FrameFacts facts;
} RigmindFactsMeta;

// Both the detector and rigmindews call these. Look the type up by name first: registering a name twice
// (the plugin and the test binary each have their own copy of this header) is a GStreamer error.
inline GType rigmind_facts_meta_api_get_type() {
    GType type = g_type_from_name("RigmindFactsMetaAPI");
    if (!type) {
        static const gchar* tags[] = {nullptr};
        type = gst_meta_api_type_register("RigmindFactsMetaAPI", tags);
    }
    return type;
}

inline gboolean rigmind_facts_meta_init(GstMeta* meta, gpointer, GstBuffer*) {
    new (&reinterpret_cast<RigmindFactsMeta*>(meta)->facts) rigmind::FrameFacts();
    return TRUE;
}

inline const GstMetaInfo* rigmind_facts_meta_get_info() {
    const GstMetaInfo* info = gst_meta_get_info("RigmindFactsMeta");
    if (!info) {
        info = gst_meta_register(rigmind_facts_meta_api_get_type(), "RigmindFactsMeta", sizeof(RigmindFactsMeta),
                                 rigmind_facts_meta_init, nullptr, nullptr);
    }
    return info;
}

// Attaches the facts for this frame. Called by the detector, once per buffer.
inline void rigmind_facts_meta_add(GstBuffer* buffer, const rigmind::FrameFacts& facts) {
    auto* meta = reinterpret_cast<RigmindFactsMeta*>(gst_buffer_add_meta(buffer, rigmind_facts_meta_get_info(), nullptr));
    meta->facts = facts;
}

G_END_DECLS
