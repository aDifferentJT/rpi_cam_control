#include <algorithm>
#include <charconv>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <gsl/span>

#include <uWebSockets/App.h>

extern "C" {
#include <gst/gst.h>
}

using namespace std::string_literals;

template <typename T, typename Deleter>
class owning_span : public gsl::span<T> {
  private:
    std::unique_ptr<T, Deleter> owner;

  public:
    owning_span(std::unique_ptr<T, Deleter> first, typename gsl::span<T>::size_type count) : gsl::span<T>{first.get(), count}, owner{std::move(first)} {}
};

namespace GLib {
  using ref_t = gpointer (*)(gpointer);
  using unref_t = void (*)(gpointer);
  
  class Property {
    private:
      GObject *object;
      char const *name;

      Property(GObject *object, char const *name) : object{object}, name{name} {}

    public:
      Property() = delete;
      Property(const Property&) = default;
      Property(Property&&) = default;
      Property& operator=(const Property&) = default;
      Property& operator=(Property&&) = default;

      void operator=(GValue value) {
        g_object_set_property(object, name, &value);
      }

      void operator=(bool value) {
        GValue g_value = G_VALUE_INIT;
        g_value_init(&g_value, G_TYPE_BOOLEAN);
        g_value_set_boolean(&g_value, value);
        *this = g_value;
      }

      void operator=(gint value) {
        GValue g_value = G_VALUE_INIT;
        g_value_init(&g_value, G_TYPE_INT);
        g_value_set_int(&g_value, value);
        *this = g_value;
      }

      void operator=(guint value) {
        GValue g_value = G_VALUE_INIT;
        g_value_init(&g_value, G_TYPE_UINT);
        g_value_set_uint(&g_value, value);
        *this = g_value;
      }

      void operator=(gfloat value) {
        GValue g_value = G_VALUE_INIT;
        g_value_init(&g_value, G_TYPE_FLOAT);
        g_value_set_float(&g_value, value);
        *this = g_value;
      }

      void operator=(char const *value) {
        GValue g_value = G_VALUE_INIT;
        g_value_init(&g_value, G_TYPE_STRING);
        g_value_set_string(&g_value, value);
        *this = g_value;
      }

      void operator=(std::string value) {
        *this = value.c_str();
      }

      void operator=(std::string_view value) {
        *this = std::string{value};
      }

      GValue g_value(GType g_type) {
        GValue g_value = G_VALUE_INIT;
        g_value_init(&g_value, g_type);
        g_object_get_property(object, name, &g_value);
        return g_value;
      }

      template <typename T>
      T get();

      template <>
      bool get() {
        auto g_value = this->g_value(G_TYPE_BOOLEAN);
        return g_value_get_boolean(&g_value);
      }

      template <>
      gint get() {
        auto g_value = this->g_value(G_TYPE_INT);
        return g_value_get_int(&g_value);
      }

      template <>
      guint get() {
        auto g_value = this->g_value(G_TYPE_UINT);
        return g_value_get_uint(&g_value);
      }

      template <>
      gfloat get() {
        auto g_value = this->g_value(G_TYPE_FLOAT);
        return g_value_get_float(&g_value);
      }

      template <>
      char const * get() {
        auto g_value = this->g_value(G_TYPE_STRING);
        return g_value_get_string(&g_value);
      }

      template <ref_t ref, unref_t unref>
      friend class Object;
  };

  template <ref_t ref = &g_object_ref, unref_t unref = &g_object_unref>
  class Object {
    protected:
      GObject *object;

    public:
      Object(GObject *object) : object{object} {}

      Object() = delete;

      Object(Object const & other) : object{other.object} {
        if (object) {
          ref(object);
        }
      }

      Object(Object&& other) {
        object = std::exchange(other.object, nullptr);
      }

      Object& operator=(Object other) {
        swap(*this, other);
        return *this;
      }

      friend void swap(Object o1, Object& o2) {
        std::swap(o1.object, o2.object);
      }

      Property operator[](char const *name) {
        return Property{object, name};
      }

      virtual ~Object() {
        if (object) {
          unref(object);
        }
      }

      auto properties() {
        auto properties_length = guint{};
        auto properties_ptr = std::unique_ptr<GParamSpec*, void (*)(gpointer)>
          { g_object_class_list_properties(G_OBJECT_GET_CLASS(object), &properties_length)
          , g_free
          };
        return owning_span{std::move(properties_ptr), gsl::narrow<int>(properties_length)};
      }

      auto property(char const *name) {
        return g_object_class_find_property(G_OBJECT_GET_CLASS(object), name);
      }
  };
}

namespace Gst {
  class Object : public GLib::Object<&gst_object_ref, &gst_object_unref> {
    protected:
      Object(GstObject *object) : GLib::Object<&gst_object_ref, &gst_object_unref>{G_OBJECT(object)} {}
  };

  class Element : public Object {
    protected:
      Element(GstElement *element) : Object{GST_OBJECT(element)} {}

    public:
      Element(char const *factoryname, char const *name) : Object{GST_OBJECT(gst_element_factory_make(factoryname, name))} {
        if (!object) { g_warning("Failed to create %s", name); }
      }

      static void link(Element src, Element dest) {
        if (!gst_element_link(GST_ELEMENT(src.object), GST_ELEMENT(dest.object))) { g_warning ("Failed to link %s to %s", src["name"].get<char const *>(), dest["name"].get<char const *>()); }
      }

      static void link_filtered(Element src, Element dest, GstCaps *filter) {
        if (!gst_element_link_filtered(GST_ELEMENT(src.object), GST_ELEMENT(dest.object), filter)) { g_warning ("Failed to link %s to %s", src["name"].get<char const *>(), dest["name"].get<char const *>()); }
      }

      void set_state(GstState state) {
        if (!gst_element_set_state(GST_ELEMENT(object), state)) { g_warning("Failed to start %s", (*this)["name"].get<char const *>()); }
      }

      friend class Bin;
  };

  class Bin : public Element {
    protected:
      Bin(GstElement *element) : Element{element} {}

    public:
      void add(Element element) {
        gst_bin_add(GST_BIN(object), GST_ELEMENT(element.object));
      }
  };

  class Pipeline : public Bin {
    public:
      Pipeline(char const *name) : Bin{gst_pipeline_new(name)} {}
  };
}

struct json_literal {
  std::string raw;
};

template <typename Res>
void writeData(Res *res, json_literal value) {
  res->write(value.raw);
}

template <typename Res>
void writeData(Res *res, char const *value) {
  res->write("\"");
  res->write(value);
  res->write("\"");
}

template <typename Res, typename Bool, std::enable_if_t<std::is_same_v<Bool, bool>, int> = 0>
void writeData(Res *res, Bool value) {
  res->write(value ? "true" : "false");
}

template <typename Res, typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0, std::enable_if_t<!std::is_same_v<T, bool>, int> = 0>
void writeData(Res *res, T value) {
  res->write(std::to_string(value));
}

template <typename Res, typename T, int n>
void writeData(Res *res, gsl::span<T, n> values) {
  res->write("[");
  for (auto& value : values) {
    writeData(res, value);
    res->write(",");
  }
  res->write("]");
}

template <typename Res, typename T, typename U, int n>
void writeData(Res *res, std::pair<gsl::span<T, n>, U (*)(T&)> transformThunk) {
  auto& [values, f] = transformThunk;
  res->write("[");
  if (!values.empty()) {
    writeData(res, f(values[0]));
    for (auto& value : values.subspan(1)) {
      res->write(",");
      writeData(res, f(value));
    }
  }
  res->write("]");
}

template <typename Res, typename T>
void writeField(Res *res, char const *field, T value) {
  res->write(",\"");
  res->write(field);
  res->write("\":");
  writeData(res, value);
}

template <typename Res, GLib::ref_t ref, GLib::unref_t unref>
void writeProperty(const char *starter, Res *res, GLib::Object<ref, unref>& object, GParamSpec *property) {
  if (G_IS_PARAM_SPEC_OBJECT(property)) { return; }

  res->write(starter);

  res->write("{\"type\":");
  writeData(res, G_PARAM_SPEC_TYPE_NAME(property));

  writeField(res, "name", g_param_spec_get_name(property));
  writeField(res, "nick", g_param_spec_get_nick(property));
  writeField(res, "blurb", g_param_spec_get_blurb(property));

  GLib::Property value = object[g_param_spec_get_name(property)];

  if (G_IS_PARAM_SPEC_BOOLEAN(property)) {
    auto boolProperty = reinterpret_cast<GParamSpecBoolean*>(property);
    writeField(res, "value", value.template get<bool>());
    writeField(res, "default_value", bool(boolProperty->default_value));
  } else if (G_IS_PARAM_SPEC_INT(property)) {
    auto intProperty = reinterpret_cast<GParamSpecInt*>(property);
    writeField(res, "value", value.template get<int>());
    writeField(res, "minimum", intProperty->minimum);
    writeField(res, "maximum", intProperty->maximum);
    writeField(res, "default_value", intProperty->default_value);
  } else if (G_IS_PARAM_SPEC_UINT(property)) {
    auto uintProperty = reinterpret_cast<GParamSpecUInt*>(property);
    writeField(res, "value", value.template get<guint>());
    writeField(res, "minimum", uintProperty->minimum);
    writeField(res, "maximum", uintProperty->maximum);
    writeField(res, "default_value", uintProperty->default_value);
  } else if (G_IS_PARAM_SPEC_FLOAT(property)) {
    auto floatProperty = reinterpret_cast<GParamSpecFloat*>(property);
    writeField(res, "value", value.template get<float>());
    writeField(res, "minimum", floatProperty->minimum);
    writeField(res, "maximum", floatProperty->maximum);
    writeField(res, "default_value", floatProperty->default_value);
    writeField(res, "epsilon", floatProperty->epsilon);
  } else if (G_IS_PARAM_SPEC_ENUM(property)) {
    auto enumProperty = reinterpret_cast<GParamSpecEnum*>(property);
    auto g_value = value.g_value(G_PARAM_SPEC_VALUE_TYPE(property));
    writeField(res, "value", g_value_get_enum(&g_value));
    writeField(res, "minimum", enumProperty->enum_class->minimum);
    writeField(res, "maximum", enumProperty->enum_class->maximum);
    writeField
      ( res
      , "values"
      , std::pair<gsl::span<GEnumValue>, json_literal (*)(GEnumValue&)>
        { gsl::span{enumProperty->enum_class->values, gsl::narrow<int>(enumProperty->enum_class->n_values)}
        , [] (auto enumValue) {
            return json_literal{"{\"value\":" + std::to_string(enumValue.value) + ",\"name\":\"" + enumValue.value_name + "\",\"nick\":\"" + enumValue.value_nick + "\"}"};
          }
        }
      );
    writeField(res, "default_value", enumProperty->default_value);
  } else if (G_IS_PARAM_SPEC_FLAGS(property)) {
    auto flagsProperty = reinterpret_cast<GParamSpecFlags*>(property);
    auto g_value = value.g_value(G_PARAM_SPEC_VALUE_TYPE(property));
    writeField(res, "value", g_value_get_flags(&g_value));
    writeField(res, "mask", flagsProperty->flags_class->mask);
    writeField
      ( res
      , "values"
      , std::pair<gsl::span<GFlagsValue>, json_literal (*)(GFlagsValue&)>
        { gsl::span{flagsProperty->flags_class->values, gsl::narrow<int>(flagsProperty->flags_class->n_values)}
        , [] (auto flagsValue) {
            return json_literal{"{\"value\":" + std::to_string(flagsValue.value) + ",\"name\":\"" + flagsValue.value_name + "\",\"nick\":\"" + flagsValue.value_nick + "\"}"};
          }
        }
      );
    writeField(res, "default_value", flagsProperty->default_value);
  } else if (G_IS_PARAM_SPEC_STRING(property)) {
    auto stringProperty = reinterpret_cast<GParamSpecString*>(property);
    writeField(res, "value", value.template get<const char *>());
    writeField(res, "default_value", stringProperty->default_value);
  } else {
    std::cerr << "Unknown property " << g_param_spec_get_name(property) << " of type: " << G_PARAM_SPEC_TYPE_NAME(property) << '\n';
  }

  res->write("}");
}

auto remove_quotes(std::string_view str) {
  return str.substr(str.find('"') + 1, str.rfind('"') - str.find('"') - 1);
}

auto parse_json_object(std::string_view json) {
  auto fields = std::vector<std::pair<std::string_view, std::string_view>>{};
  try {
    json = json.substr(json.find_first_not_of(" \t\n"));

    if (json[0] != '{') { throw std::out_of_range{""}; }

    while (json[0] != '}') {
      json = json.substr(1);
  
      auto const field_name_end = json.find(':');
      auto const field_name_quoted = json.substr(0, field_name_end);
      auto const field_name = remove_quotes(field_name_quoted);
      json = json.substr(field_name_end + 1);

      json = json.substr(json.find_first_not_of(" \t\n"));
  
      auto const value_end = json[0] == '"' ? json.find('"', 1) + 1 : json.find_first_of(",}");
      auto const value = json.substr(0, value_end);
      json = json.substr(value_end);

      json = json.substr(json.find_first_not_of(" \t\n"));

      fields.emplace_back(field_name, value);
    }
  } catch (std::out_of_range const &) {
    fields.clear();
  }
  return [fields] (auto field_name) {
      return std::find_if(fields.begin(), fields.end(), [&] (auto& field) { return field.first == field_name; })->second;
    };
}

template <typename Object>
void parse_set_property(Object object, std::string_view json) {
  auto const fields = parse_json_object(json);

  auto const name = std::string{remove_quotes(fields("name"))};
  auto const type = remove_quotes(fields("type"));
  auto const value_s = fields("value");

  auto property = object[name.c_str()];

  if (type == "GParamBoolean") {
    if (value_s == "true") {
      property = true;
    } else if (value_s == "false") {
      property = false;
    } else {
      std::cerr << "Invalid bool: " << value_s << '\n';
    }
  } else if (type == "GParamInt") {
    auto value = gint{};
    auto res = std::from_chars(value_s.begin(), value_s.end(), value);

    if (res.ptr == value_s.end()) {
      property = value;
    } else {
      std::cerr << "Invalid int: " << value_s << '\n';
    }
  } else if (type == "GParamUInt") {
    auto value = guint{};
    auto res = std::from_chars(value_s.begin(), value_s.end(), value);

    if (res.ptr == value_s.end()) {
      property = value;
    } else {
      std::cerr << "Invalid int: " << value_s << '\n';
    }
  } else if (type == "GParamFloat") {
    try {
      property = std::stof(std::string{value_s});
    } catch (std::invalid_argument const &) {
      std::cerr << "Invalid float: " << value_s << '\n';
    } catch (std::out_of_range const &) {
      std::cerr << "Invalid float: " << value_s << '\n';
    }
  } else if (type == "GParamEnum") {
    GValue g_value = G_VALUE_INIT;
    g_value_init(&g_value, G_PARAM_SPEC_VALUE_TYPE(object.property(name.c_str())));

    auto value = gint{};
    auto res = std::from_chars(value_s.begin(), value_s.end(), value);

    if (res.ptr == value_s.end()) {
      g_value_set_enum(&g_value, value);
      property = g_value;
    } else {
      std::cerr << "Invalid enum: " << value_s << '\n';
    }
  } else if (type == "GParamFlags") {
    GValue g_value = G_VALUE_INIT;
    g_value_init(&g_value, G_PARAM_SPEC_VALUE_TYPE(object.property(name.c_str())));

    auto value = guint{};
    auto res = std::from_chars(value_s.begin(), value_s.end(), value);

    if (res.ptr == value_s.end()) {
      g_value_set_flags(&g_value, value);
      property = g_value;
    } else {
      std::cerr << "Invalid flags: " << value_s << '\n';
    }
  } else if (type == "GParamString") {
    property = remove_quotes(value_s);
  } else {
    std::cerr << "Unrecognised type: " << type << '\n';
  }
}

int main(int argc, char **argv) {
  // init
  gst_init (&argc, &argv);

  auto loop = std::unique_ptr<GMainLoop, void (*)(GMainLoop*)>{g_main_loop_new(nullptr, FALSE), g_main_loop_unref};

  // create pipeline
  auto pipeline = Gst::Pipeline{"pipeline"};

  // create elements
  auto rpicamsrc = Gst::Element{"rpicamsrc", "rpicamsrc"};
  rpicamsrc["bitrate"] = 1000000;
  rpicamsrc["keyframe-interval"] = 30;
  rpicamsrc["preview"] = false;

  auto rtph264pay = Gst::Element{"rtph264pay", "rtph264pay"};

  auto udpsink = Gst::Element{"udpsink", "udpsink"};
  udpsink["host"] = "192.168.16.61";
  udpsink["port"] = 5000;

  /* must add elements to pipeline before linking them */
  pipeline.add(rpicamsrc);
  pipeline.add(rtph264pay);
  pipeline.add(udpsink);

  auto filter = gst_caps_new_simple
    ( "video/x-h264"
    //, "format", G_TYPE_STRING, "I420"
    , "width", G_TYPE_INT, 1280
    , "height", G_TYPE_INT, 720
    , "framerate", GST_TYPE_FRACTION, 30, 1
    , NULL
    );

  /* link */
  Gst::Element::link_filtered(rpicamsrc, rtph264pay, filter);
  Gst::Element::link(rtph264pay, udpsink);

  pipeline.set_state(GST_STATE_PLAYING);

  auto web_thread = std::thread
    ( [&] () {
        auto app = uWS::App{};
        app.get
          ( "/properties"
          , [&] (auto *res, auto *req) {
              res->cork
                ( [&] () {
                    res->writeHeader("Access-Control-Allow-Origin", "*");

                    auto properties = rpicamsrc.properties();

                    if (properties.empty()) {
                      res->write("[]");
                    } else {
                      writeProperty("[", res, rpicamsrc, properties[0]);
                      for (auto& property : properties.subspan(1)) {
                        writeProperty(",", res, rpicamsrc, property);
                      }
                      res->write("]");
                    }
                    res->end();
                  }
                );
            }
          );
        app.options
          ( "/set_property"
          , [] (auto *res, auto *req) {
              res->writeHeader("Access-Control-Allow-Headers", "*");
              res->writeHeader("Access-Control-Allow-Methods", "*");
              res->writeHeader("Access-Control-Allow-Origin", "*");
              res->end();
            }
          );
        app.post
          ( "/set_property"
          , [&] (auto *res, auto *req) mutable {
              res->writeHeader("Access-Control-Allow-Origin", "*");
              res->onData
                ( [&] (std::string_view data, bool fin) {
                    parse_set_property(rpicamsrc, data);
                  }
                );
              res->writeStatus("204 No Content");
              res->end();
            }
          );
        app.listen(9001, [](auto *listenSocket) {
          if (listenSocket) {
            std::cout << "Listening for connections..." << std::endl;
          }
        });
        app.run();
      }
    );

  // Iterate
  g_print("Running...\n");
  g_main_loop_run(loop.get());

  // Out of the main loop, clean up nicely
  g_print("Returned, stopping playback\n");
  pipeline.set_state(GST_STATE_NULL);
}
