// rigmindews: GStreamer element that runs the EWS alarm state machine on every frame.
//
// Threading: transform_ip runs on the streaming thread of one pipeline branch. The state machine is
// touched only there, so it needs no lock. A state change is posted to the bus with
// gst_element_post_message, which is thread-safe and never waits for the pipeline's main loop.
// The one value set from another thread ("zoom") is a std::atomic<bool>.
// The frame is passed through unchanged (in place), so a slow consumer never delays the live branch.
#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <atomic>
#include <new>
#include "rigmind_facts_meta.h"

namespace {

struct GstRigmindEws {
    GstBaseTransform parent;
    gchar* cam_id;
    gchar* condition;            // alert text and place, set before PLAYING
    gchar* place;
    std::atomic<bool> zoom;      // set from any thread
    bool zoom_active;            // streaming thread only
    rigmind::AlarmStateMachine* machine;
    guint64 frame;
};

struct GstRigmindEwsClass {
    GstBaseTransformClass parent_class;
};

enum { PROP_0, PROP_CAM_ID, PROP_ZOOM, PROP_CONDITION, PROP_PLACE };

// Names the pipeline and host compare against: "None", "Yellow", "Orange", "Red".
const char* state_name(rigmind::AlarmState state) {
    switch (state) {
        case rigmind::AlarmState::Red: return "Red";
        case rigmind::AlarmState::Orange: return "Orange";
        case rigmind::AlarmState::Yellow: return "Yellow";
        default: return "None";
    }
}

const char* severity_for(rigmind::AlarmState state) {
    switch (state) {
        case rigmind::AlarmState::Red: return "Critical";
        case rigmind::AlarmState::Orange: return "Warning";
        case rigmind::AlarmState::Yellow: return "Advisory";
        default: return "None";
    }
}

GstRigmindEws* as_ews(gpointer element) { return reinterpret_cast<GstRigmindEws*>(element); }

GstFlowReturn transform_ip(GstBaseTransform* base, GstBuffer* buffer) {
    GstRigmindEws* self = as_ews(base);

    // Zoom changes restart the counters, as IDSS setThresholds() does.
    const bool zoom = self->zoom.load(std::memory_order_relaxed);
    if (zoom != self->zoom_active) {
        delete self->machine;
        self->machine = new rigmind::AlarmStateMachine(rigmind::AlarmThresholds::for_zoom(zoom));
        self->zoom_active = zoom;
    }

    // A frame without facts counts as "nothing seen", which lets the counters fall.
    rigmind::FrameFacts facts{};
    if (auto* meta = reinterpret_cast<RigmindFactsMeta*>(gst_buffer_get_meta(buffer, rigmind_facts_meta_api_get_type()))) {
        facts = meta->facts;
    }

    const rigmind::AlarmState before = self->machine->current();
    const rigmind::AlarmState after = self->machine->step(facts);
    self->frame++;

    if (after != before) {
        GstStructure* body = gst_structure_new(
            "rigmind-alarm",
            "cam", G_TYPE_STRING, self->cam_id ? self->cam_id : "",
            "state", G_TYPE_STRING, state_name(after),
            "previous", G_TYPE_STRING, state_name(before),
            "severity", G_TYPE_STRING, severity_for(after),
            "condition", G_TYPE_STRING, self->condition ? self->condition : "",
            "place", G_TYPE_STRING, self->place ? self->place : "",
            "frame", G_TYPE_UINT64, self->frame,
            "pts", G_TYPE_UINT64, GST_BUFFER_PTS(buffer),
            nullptr);
        gst_element_post_message(GST_ELEMENT(self), gst_message_new_application(GST_OBJECT(self), body));
    }
    return GST_FLOW_OK;
}

void set_property(GObject* object, guint id, const GValue* value, GParamSpec* spec) {
    GstRigmindEws* self = as_ews(object);
    switch (id) {
        case PROP_CAM_ID:
            g_free(self->cam_id);
            self->cam_id = g_value_dup_string(value);
            break;
        case PROP_ZOOM:
            self->zoom.store(g_value_get_boolean(value), std::memory_order_relaxed);
            break;
        case PROP_CONDITION:
            g_free(self->condition);
            self->condition = g_value_dup_string(value);
            break;
        case PROP_PLACE:
            g_free(self->place);
            self->place = g_value_dup_string(value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

void get_property(GObject* object, guint id, GValue* value, GParamSpec* spec) {
    GstRigmindEws* self = as_ews(object);
    switch (id) {
        case PROP_CAM_ID:
            g_value_set_string(value, self->cam_id);
            break;
        case PROP_ZOOM:
            g_value_set_boolean(value, self->zoom.load(std::memory_order_relaxed));
            break;
        case PROP_CONDITION:
            g_value_set_string(value, self->condition);
            break;
        case PROP_PLACE:
            g_value_set_string(value, self->place);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
}

void instance_init(GTypeInstance* instance, gpointer) {
    GstRigmindEws* self = as_ews(instance);
    new (&self->zoom) std::atomic<bool>(false);
    self->zoom_active = false;
    self->machine = new rigmind::AlarmStateMachine(rigmind::AlarmThresholds::for_zoom(false));
    self->cam_id = nullptr;
    self->condition = nullptr;
    self->place = nullptr;
    self->frame = 0;
}

void finalize(GObject* object) {
    GstRigmindEws* self = as_ews(object);
    delete self->machine;
    self->zoom.~atomic();
    g_free(self->cam_id);
    g_free(self->condition);
    g_free(self->place);
    G_OBJECT_CLASS(g_type_class_peek_parent(G_OBJECT_GET_CLASS(object)))->finalize(object);
}

void class_init(gpointer klass, gpointer) {
    GObjectClass* gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass* transform_class = GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = set_property;
    gobject_class->get_property = get_property;
    gobject_class->finalize = finalize;
    g_object_class_install_property(gobject_class, PROP_CAM_ID,
        g_param_spec_string("cam-id", "Camera id", "Camera id put on each alarm message", "",
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(gobject_class, PROP_CONDITION,
        g_param_spec_string("condition", "Condition", "Alert text for the raised alarm", "",
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(gobject_class, PROP_PLACE,
        g_param_spec_string("place", "Place", "Rig and area shown with the alarm", "",
                            static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));
    g_object_class_install_property(gobject_class, PROP_ZOOM,
        g_param_spec_boolean("zoom", "Zoom mode", "Use the longer IDSS zoom counters", FALSE,
                             static_cast<GParamFlags>(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

    gst_element_class_set_static_metadata(element_class, "RigMind EWS alarm", "Filter/Analyzer/Video",
                                          "Runs the EWS alarm state machine on each frame and posts transitions to the bus",
                                          "RigMind");
    static GstStaticPadTemplate pad_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
    gst_element_class_add_static_pad_template(element_class, &pad_template);
    static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
    gst_element_class_add_static_pad_template(element_class, &src_template);

    transform_class->transform_ip = transform_ip;
}

GType ews_get_type() {
    static GType type = 0;
    if (!type) {
        static const GTypeInfo info = {
            sizeof(GstRigmindEwsClass), nullptr, nullptr, class_init, nullptr, nullptr,
            sizeof(GstRigmindEws), 0, instance_init, nullptr,
        };
        type = g_type_register_static(GST_TYPE_BASE_TRANSFORM, "GstRigmindEws", &info, static_cast<GTypeFlags>(0));
    }
    return type;
}

gboolean plugin_init(GstPlugin* plugin) {
    return gst_element_register(plugin, "rigmindews", GST_RANK_NONE, ews_get_type());
}

} // namespace

#ifndef PACKAGE
#define PACKAGE "rigmind"
#endif
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, rigmindews, "RigMind EWS alarm element", plugin_init,
                  "1.0.0", "proprietary", "RigMind", "https://satyaki.co.in")
