#include "asset/materialx.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include "core/log.h"

namespace rx::asset {
namespace {

// Value of an attribute key="..." inside a single tag's text. The match has to
// start at a word boundary: `name="` also occurs inside `nodename="`, and a
// document that writes the connection first (which is the order MaterialX
// exporters emit) then hands every input the name of the node it connects to.
std::string Attr(const std::string& tag, const char* key) {
  const std::string pat = std::string(key) + "=\"";
  size_t p = 0;
  while ((p = tag.find(pat, p)) != std::string::npos) {
    const char before = p == 0 ? '<' : tag[p - 1];
    p += pat.size();
    if (before != ' ' && before != '\t' && before != '\n' && before != '\r' && before != '<') {
      continue;
    }
    const size_t e = tag.find('"', p);
    return e == std::string::npos ? std::string() : tag.substr(p, e - p);
  }
  return "";
}

// MaterialX vector/color values are comma separated; parse up to n floats.
void ParseFloats(const std::string& value, f32* out, int n) {
  std::string s = value;
  for (char& c : s) {
    if (c == ',') c = ' ';
  }
  std::istringstream ss(s);
  f32 v;
  for (int i = 0; i < n && (ss >> v); ++i) out[i] = v;
}

struct Input {
  std::string name;
  std::string value;     // a constant
  std::string nodename;  // a connection to another node in the same scope
  std::string nodegraph;  // a connection into a nodegraph's output
  std::string output;     // which output of that nodegraph
};

struct Node {
  std::string category;  // "tiledimage", "normalmap", "open_pbr_surface", "output", ...
  std::string name;
  std::string nodename;  // an <output> element's source node
  std::vector<Input> inputs;
};

// The document as a flat list of elements, which is all this needs: MaterialX
// node names are document-unique, so nesting (a <nodegraph> wrapping its nodes,
// which is how a 1.38 document is usually written, against the flat root list
// ambientCG's 1.39 exporter emits) carries no information a lookup by name does
// not already have. <input> elements attach to the last element that opened and
// did not close itself, which is the node they belong to under either layout.
std::vector<Node> ScanElements(const std::string& doc) {
  std::vector<Node> nodes;
  size_t p = 0;
  size_t current = std::string::npos;  // an index, since push_back moves the storage
  while ((p = doc.find('<', p)) != std::string::npos) {
    const size_t end = doc.find('>', p);
    if (end == std::string::npos) break;
    const std::string tag = doc.substr(p, end - p);
    p = end + 1;
    if (tag.size() < 2 || tag[1] == '/' || tag[1] == '?' || tag[1] == '!') continue;

    size_t name_end = tag.find_first_of(" \t\r\n", 1);
    if (name_end == std::string::npos) name_end = tag.size();
    const std::string category = tag.substr(1, name_end - 1);
    const bool self_closing = tag.back() == '/';

    if (category == "input") {
      if (current == std::string::npos) continue;
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

const Node* FindNode(const std::vector<Node>& nodes, const std::string& name) {
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
std::string ResolveImage(const std::vector<Node>& nodes, const Input& input, std::string* why) {
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
std::string ResolveAgainstDocument(const std::string& document, const std::string& file) {
  const std::filesystem::path relative(file);
  if (file.empty() || relative.is_absolute()) return file;
  const std::filesystem::path dir = std::filesystem::path(document).parent_path();
  if (dir.empty()) return file;
  return (dir / relative).lexically_normal().string();
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

std::string Canonical(const std::string& name) {
  for (const InputAlias& alias : kInputAliases) {
    if (name == alias.name) return alias.canonical;
  }
  return name;
}

// Which map slot a surface input fills when it is connected to an image.
auto MapSlot(const std::string& canonical) -> std::string MaterialXMaps::* {
  if (canonical == "base_color") return &MaterialXMaps::base_color;
  if (canonical == "normal") return &MaterialXMaps::normal;
  if (canonical == "specular_roughness") return &MaterialXMaps::roughness;
  if (canonical == "metalness") return &MaterialXMaps::metallic;
  if (canonical == "occlusion") return &MaterialXMaps::occlusion;
  if (canonical == "emission_color") return &MaterialXMaps::emissive;
  return nullptr;
}

}  // namespace

bool LoadMaterialX(const std::string& path, Material* out, MaterialXMaps* maps) {
  std::ifstream file(path);
  if (!file) {
    RX_WARN("materialx: cannot open {}", path);
    return false;
  }
  std::stringstream buf;
  buf << file.rdbuf();
  const std::string doc = buf.str();

  // MaterialX lets a document prefix every filename it names. "./" is the no-op
  // ambientCG writes and the only one honoured; anything else would put the
  // maps somewhere the resolution below does not look, and losing a whole
  // texture set to one unread attribute is worth saying out loud.
  if (const size_t root = doc.find("<materialx"); root != std::string::npos) {
    const size_t end = doc.find('>', root);
    const std::string prefix =
        Attr(doc.substr(root, end == std::string::npos ? end : end - root), "fileprefix");
    if (!prefix.empty() && prefix != "./") {
      RX_WARN("materialx: {}: fileprefix=\"{}\" is not applied; filenames resolve against the "
              "document's own directory", path, prefix);
    }
  }

  const std::vector<Node> nodes = ScanElements(doc);
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

  // Surface inputs that are not 1:1 engine fields get combined below.
  f32 base_weight = 1.0f;
  f32 sheen_weight = 0.0f, sheen_color[3] = {1, 1, 1};
  f32 emission_weight = 0.0f, emission_color[3] = {1, 1, 1};
  f32 thin_film_thickness = 0.0f;

  for (const Input& input : surface->inputs) {
    const std::string name = Canonical(input.name);
    if (input.value.empty()) {
      // A connected input. Resolving it to a texture is the whole point of
      // pointing rx at a library document; one this build cannot follow is
      // named rather than dropped in silence, since the render it produces
      // (a flat colour) looks exactly like a material authored that way.
      std::string why;
      const std::string image = ResolveImage(nodes, input, &why);
      std::string MaterialXMaps::*slot = MapSlot(name);
      if (image.empty()) {
        RX_WARN("materialx: {}: input '{}' {}; that map is DROPPED", path, input.name, why);
      } else if (!slot) {
        RX_WARN("materialx: {}: input '{}' is an image ({}) and this engine has no texture slot "
                "for it; that map is DROPPED", path, input.name, image);
      } else if (maps) {
        maps->*slot = ResolveAgainstDocument(path, image);
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
    }
  }

  for (int i = 0; i < 3; ++i) {
    out->base_color_factor[i] *= base_weight;
    out->sheen_color[i] = sheen_color[i] * sheen_weight;
    out->emissive_factor[i] = emission_color[i] * emission_weight;
  }
  if (thin_film_thickness > 0.0f) {
    out->iridescence = 1.0f;
    out->iridescence_thickness = thin_film_thickness;
  }
  if (out->transmission > 0.0f) out->alpha_mode = AlphaMode::kBlend;
  RX_INFO("materialx: loaded <{}> from {}", surface->category, path);
  return true;
}

}  // namespace rx::asset
