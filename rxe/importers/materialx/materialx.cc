#include "rxe/importers/materialx/materialx.h"


#include "base/containers/vector.h"
#include "base/strings/xstring.h"
#include "foundation/files/file_system.h"
#include "foundation/logging/log.h"
#include "foundation/strings/text_reader.h"

namespace rx::asset {
namespace {

// Value of an attribute key="..." inside a single tag's text. The match has to
// start at a word boundary: `name="` also occurs inside `nodename="`, and a
// document that writes the connection first (which is the order MaterialX
// exporters emit) then hands every input the name of the node it connects to.
base::String Attr(const base::String& tag, const char* key) {
  const base::String pat = base::String(key) + "=\"";
  size_t p = 0;
  while ((p = tag.find(pat, p)) != base::String::npos) {
    const char before = p == 0 ? '<' : tag[p - 1];
    p += pat.size();
    if (before != ' ' && before != '\t' && before != '\n' && before != '\r' && before != '<') {
      continue;
    }
    const size_t e = tag.find('"', p);
    return e == base::String::npos ? base::String() : tag.substr(p, e - p);
  }
  return "";
}

// MaterialX vector/color values are comma separated; parse up to n floats.
void ParseFloats(const base::String& value, f32* out, int n) {
  base::String s = value;
  for (char& c : s) {
    if (c == ',') c = ' ';
  }
  TokenReader ss(s);
  f32 v;
  for (int i = 0; i < n && ss.Next(&v); ++i) out[i] = v;
}

struct Input {
  base::String name;
  base::String value;     // a constant
  base::String nodename;  // a connection to another node in the same scope
  base::String nodegraph;  // a connection into a nodegraph's output
  base::String output;     // which output of that nodegraph
};

struct Node {
  base::String category;  // "tiledimage", "normalmap", "open_pbr_surface", "output", ...
  base::String name;
  base::String nodename;  // an <output> element's source node
  base::Vector<Input> inputs;
};

// The document as a flat list of elements, which is all this needs: MaterialX
// node names are document-unique, so nesting (a <nodegraph> wrapping its nodes,
// which is how a 1.38 document is usually written, against the flat root list
// ambientCG's 1.39 exporter emits) carries no information a lookup by name does
// not already have. <input> elements attach to the last element that opened and
// did not close itself, which is the node they belong to under either layout.
base::Vector<Node> ScanElements(const base::String& doc) {
  base::Vector<Node> nodes;
  size_t p = 0;
  size_t current = base::String::npos;  // an index, since push_back moves the storage
  while ((p = doc.find('<', p)) != base::String::npos) {
    const size_t end = doc.find('>', p);
    if (end == base::String::npos) break;
    const base::String tag = doc.substr(p, end - p);
    p = end + 1;
    if (tag.size() < 2 || tag[1] == '/' || tag[1] == '?' || tag[1] == '!') continue;

    size_t name_end = tag.find_first_of(" \t\r\n", 1);
    if (name_end == base::String::npos) name_end = tag.size();
    const base::String category = tag.substr(1, name_end - 1);
    const bool self_closing = tag.back() == '/';

    if (category == "input") {
      if (current == base::String::npos) continue;
      nodes[current].inputs.push_back({Attr(tag, "name"), Attr(tag, "value"),
                                       Attr(tag, "nodename"), Attr(tag, "nodegraph"),
                                       Attr(tag, "output")});
      continue;
    }
    nodes.push_back({category, Attr(tag, "name"), Attr(tag, "nodename"), {}});
    // A self-closing element has no inputs to collect, and letting it take over
    // would hand it the inputs of whatever node opens next.
    if (!self_closing) current = nodes.size() - 1;
  }
  return nodes;
}

const Node* FindNode(const base::Vector<Node>& nodes, const base::String& name) {
  if (name.empty()) return nullptr;
  for (const Node& node : nodes) {
    if (node.name == name) return &node;
  }
  return nullptr;
}

const Input* FindInput(const Node& node, const char* name) {
  for (const Input& input : node.inputs) {
    if (input.name == name) return &input;
  }
  return nullptr;
}

// The image file a connected input ends at, or empty with `why` saying what
// stopped the walk. Two hops are enough for every texture set in the wild:
// straight to an <image>/<tiledimage>, or through a <normalmap> to the image
// feeding it. Anything else is a graph this build does not evaluate, and saying
// which node it gave up on is the difference between a fixable document and a
// material that came back flat for no stated reason.
base::String ResolveImage(const base::Vector<Node>& nodes, const Input& input, base::String* why) {
  const Node* node = FindNode(nodes, input.nodename);
  if (!node && !input.nodegraph.empty()) {
    // <input nodegraph="NG" output="out_color"/>: the graph's <output> element
    // names the node that actually produces the value.
    if (const Node* out = FindNode(nodes, input.output)) node = FindNode(nodes, out->nodename);
  }
  if (!node) {
    *why = "connects to '" + (input.nodename.empty() ? input.output : input.nodename) +
           "', which is not a node in this document";
    return {};
  }
  for (int hop = 0; hop < 2; ++hop) {
    if (node->category == "image" || node->category == "tiledimage") {
      const Input* file = FindInput(*node, "file");
      if (!file || file->value.empty()) {
        *why = "reaches <" + node->category + " name=\"" + node->name +
               "\">, which names no file";
        return {};
      }
      return file->value;
    }
    // A normalmap node only converts the tangent-space encoding the engine
    // already expects, so following it and taking the image is exact.
    if (node->category == "normalmap") {
      const Input* in = FindInput(*node, "in");
      const Node* next = in ? FindNode(nodes, in->nodename) : nullptr;
      if (!next) break;
      node = next;
      continue;
    }
    break;
  }
  *why = "reaches <" + node->category + " name=\"" + node->name +
         "\">, and only <image>, <tiledimage> and <normalmap> are evaluated here";
  return {};
}

// A document-relative filename made absolute against the document's directory.
base::String ResolveAgainstDocument(const base::String& document, const base::String& file) {
  if (file.empty() || fs::IsAbsolute(file)) return file;
  const base::StringRef dir = fs::ParentPath(document);
  if (dir.empty()) return file;
  return fs::LexicallyNormal(fs::Join(dir, file));
}

// standard_surface and open_pbr_surface name the same lobes differently, and a
// texture library ships whichever its exporter emits (ambientCG: open_pbr).
// Reading both under one table is what keeps that from being the author's
// problem; the two vocabularies do not collide on any name.
struct InputAlias {
  const char* name;
  const char* canonical;
};
constexpr InputAlias kInputAliases[] = {
    {"base_weight", "base"},
    {"base_metalness", "metalness"},
    {"specular_ior", "specular_IOR"},
    {"specular_roughness_anisotropy", "specular_anisotropy"},
    {"coat_weight", "coat"},
    {"fuzz_weight", "sheen"},
    {"fuzz_color", "sheen_color"},
    {"fuzz_roughness", "sheen_roughness"},
    {"subsurface_weight", "subsurface"},
    {"transmission_weight", "transmission"},
    {"emission_luminance", "emission"},
    {"geometry_normal", "normal"},
};

base::String Canonical(const base::String& name) {
  for (const InputAlias& alias : kInputAliases) {
    if (name == alias.name) return alias.canonical;
  }
  return name;
}

// Which map slot a surface input fills when it is connected to an image.
auto MapSlot(const base::String& canonical) -> base::String MaterialXMaps::* {
  if (canonical == "base_color") return &MaterialXMaps::base_color;
  if (canonical == "normal") return &MaterialXMaps::normal;
  if (canonical == "specular_roughness") return &MaterialXMaps::roughness;
  if (canonical == "metalness") return &MaterialXMaps::metallic;
  if (canonical == "occlusion") return &MaterialXMaps::occlusion;
  if (canonical == "emission_color") return &MaterialXMaps::emissive;
  return nullptr;
}

}  // namespace

bool LoadMaterialX(const base::String& path, Material* out, MaterialXMaps* maps) {
  base::String doc;
  if (!fs::ReadTextFile(path, &doc)) {
    RX_WARN("materialx: cannot open {}", path);
    return false;
  }

  // MaterialX lets a document prefix every filename it names. "./" is the no-op
  // ambientCG writes and the only one honoured; anything else would put the
  // maps somewhere the resolution below does not look, and losing a whole
  // texture set to one unread attribute is worth saying out loud.
  if (const size_t root = doc.find("<materialx"); root != base::String::npos) {
    const size_t end = doc.find('>', root);
    const base::String prefix =
        Attr(doc.substr(root, end == base::String::npos ? end : end - root), "fileprefix");
    if (!prefix.empty() && prefix != "./") {
      RX_WARN("materialx: {}: fileprefix=\"{}\" is not applied; filenames resolve against the "
              "document's own directory", path, prefix);
    }
  }

  const base::Vector<Node> nodes = ScanElements(doc);
  const Node* surface = nullptr;
  for (const Node& node : nodes) {
    if (node.category == "standard_surface" || node.category == "open_pbr_surface") {
      surface = &node;
      break;
    }
  }
  if (!surface) {
    RX_WARN("materialx: {} has no standard_surface or open_pbr_surface node", path);
    return false;
  }

  // The alias table only unifies spelling. OpenPBR also differs in defaults,
  // units and parametrization, and taking the glTF-derived engine value for an
  // input the document leaves unauthored would shade something nobody wrote.
  const bool open_pbr = surface->category == "open_pbr_surface";
  if (open_pbr) ApplyOpenPbrDefaults(out);

  // Surface inputs that are not 1:1 engine fields get combined below.
  f32 base_weight = 1.0f;
  f32 sheen_weight = 0.0f, sheen_color[3] = {1, 1, 1};
  f32 emission_weight = 0.0f, emission_color[3] = {1, 1, 1};
  // standard_surface: nanometres, 0 = no film. OpenPBR: micrometres, weighted
  // by thin_film_weight, with a spec default of 0.5.
  f32 thin_film_thickness = open_pbr ? 0.5f : 0.0f;
  f32 opacity = 1.0f;

  for (const Input& input : surface->inputs) {
    const base::String name = Canonical(input.name);
    if (input.value.empty()) {
      // A connected input. Resolving it to a texture is the whole point of
      // pointing rx at a library document; one this build cannot follow is
      // named rather than dropped in silence, since the render it produces
      // (a flat colour) looks exactly like a material authored that way.
      base::String why;
      const base::String image = ResolveImage(nodes, input, &why);
      base::String MaterialXMaps::*slot = MapSlot(name);
      if (image.empty()) {
        RX_WARN("materialx: {}: input '{}' {}; that map is DROPPED", path, input.name, why);
      } else if (!slot) {
        RX_WARN("materialx: {}: input '{}' is an image ({}) and this engine has no texture slot "
                "for it; that map is DROPPED", path, input.name, image);
      } else if (maps) {
        maps->*slot = ResolveAgainstDocument(path, image);
        // The shader multiplies a map by its factor, so the map has to carry
        // the value alone. Left at the document default, an OpenPBR base
        // colour map renders at 0.8x and a metalness map at 0x.
        if (name == "base_color") {
          for (int i = 0; i < 3; ++i) out->base_color_factor[i] = 1.0f;
        } else if (name == "specular_roughness") {
          out->roughness_factor = 1.0f;
        } else if (name == "metalness") {
          out->metallic_factor = 1.0f;
        }
      }
      continue;
    }

    if (name == "base") {
      ParseFloats(input.value, &base_weight, 1);
    } else if (name == "base_color") {
      ParseFloats(input.value, out->base_color_factor, 3);
    } else if (name == "metalness") {
      ParseFloats(input.value, &out->metallic_factor, 1);
    } else if (name == "specular_roughness") {
      ParseFloats(input.value, &out->roughness_factor, 1);
    } else if (name == "specular_IOR") {
      ParseFloats(input.value, &out->ior, 1);
    } else if (name == "specular_anisotropy") {
      ParseFloats(input.value, &out->anisotropy, 1);
    } else if (name == "coat") {
      ParseFloats(input.value, &out->clearcoat, 1);
    } else if (name == "coat_roughness") {
      ParseFloats(input.value, &out->clearcoat_roughness, 1);
    } else if (name == "sheen") {
      ParseFloats(input.value, &sheen_weight, 1);
    } else if (name == "sheen_color") {
      ParseFloats(input.value, sheen_color, 3);
    } else if (name == "sheen_roughness") {
      ParseFloats(input.value, &out->sheen_roughness, 1);
    } else if (name == "subsurface") {
      ParseFloats(input.value, &out->subsurface, 1);
    } else if (name == "subsurface_color") {
      ParseFloats(input.value, out->subsurface_color, 3);
    } else if (name == "transmission") {
      ParseFloats(input.value, &out->transmission, 1);
    } else if (name == "emission") {
      ParseFloats(input.value, &emission_weight, 1);
    } else if (name == "emission_color") {
      ParseFloats(input.value, emission_color, 3);
    } else if (name == "thin_film_thickness") {
      ParseFloats(input.value, &thin_film_thickness, 1);
    } else if (open_pbr) {
      // Inputs only OpenPBR has, or where standard_surface's input of the same
      // name means something else.
      if (name == "base_diffuse_roughness") {
        ParseFloats(input.value, &out->base_diffuse_roughness, 1);
      } else if (name == "specular_weight") {
        ParseFloats(input.value, &out->specular_weight, 1);
      } else if (name == "specular_color") {
        ParseFloats(input.value, out->openpbr_specular_color, 3);
      } else if (name == "coat_color") {
        ParseFloats(input.value, out->coat_color, 3);
      } else if (name == "coat_ior") {
        ParseFloats(input.value, &out->coat_ior, 1);
      } else if (name == "coat_darkening") {
        ParseFloats(input.value, &out->coat_darkening, 1);
      } else if (name == "thin_film_weight") {
        ParseFloats(input.value, &out->iridescence, 1);
      } else if (name == "thin_film_ior") {
        ParseFloats(input.value, &out->thin_film_ior, 1);
      } else if (name == "geometry_opacity") {
        ParseFloats(input.value, &opacity, 1);
      }
    }
  }

  for (int i = 0; i < 3; ++i) {
    out->base_color_factor[i] *= base_weight;
    out->sheen_color[i] = sheen_color[i] * sheen_weight;
    out->emissive_factor[i] = emission_color[i] * emission_weight;
  }
  if (open_pbr) {
    out->iridescence_thickness = thin_film_thickness * 1000.0f;
    out->anisotropy = OpenPbrAnisotropyToEngine(out->anisotropy);
    out->base_color_factor[3] = opacity;
  } else if (thin_film_thickness > 0.0f) {
    out->iridescence = 1.0f;
    out->iridescence_thickness = thin_film_thickness;
  }
  if (out->transmission > 0.0f || out->base_color_factor[3] < 1.0f) {
    out->alpha_mode = AlphaMode::kBlend;
  }
  RX_INFO("materialx: loaded <{}> from {}", surface->category, path);
  return true;
}

}  // namespace rx::asset
