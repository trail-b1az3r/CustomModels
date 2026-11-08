#include "VivifySerializedFile.hpp"

#include <algorithm>
#include <cstring>
#include <array>
#include <map>
#include <set>
#include <tuple>

namespace SerializedFileParse {
namespace {

// Unity ClassID for UnityEngine.Shader.
constexpr int32_t kClassIdShader = 48;
constexpr int32_t kClassIdTexture2D = 28;

// Type-tree node flags. Bit 0 of typeFlags marks a node whose children are the
// [size, data] pair of an array; bit 14 of metaFlag requests 4-byte alignment
// after the field has been read.
constexpr uint8_t kTypeFlagIsArray = 0x1;
constexpr uint32_t kMetaFlagAlignBytes = 0x4000;

// A string offset with the high bit set indexes Unity's built-in common string
// table rather than this file's own buffer.
constexpr uint32_t kCommonStringBit = 0x80000000u;

// Reader over serialized-file metadata. The header itself is big-endian; every
// field after it follows the file's endianness flag, which is little-endian in
// practice for every platform Unity still ships.
class Reader {
 public:
  Reader(uint8_t const* data, size_t size) : _data(data), _size(size) {}

  bool ok() const { return _ok; }
  size_t position() const { return _pos; }
  uint8_t const* base() const { return _data; }
  size_t remaining() const { return _pos <= _size ? _size - _pos : 0; }
  void setBigEndian(bool big) { _big = big; }

  void seek(size_t pos) {
    if (pos > _size) { _ok = false; return; }
    _pos = pos;
  }

  void skip(size_t count) {
    if (count > remaining()) { _ok = false; _pos = _size; return; }
    _pos += count;
  }

  void align4() {
    size_t const rem = _pos % 4;
    if (rem != 0) skip(4 - rem);
  }

  uint8_t u8() {
    if (remaining() < 1) { _ok = false; return 0; }
    return _data[_pos++];
  }

  uint16_t u16() {
    uint8_t b[2];
    if (!raw(b, 2)) return 0;
    return _big ? static_cast<uint16_t>((b[0] << 8) | b[1])
                : static_cast<uint16_t>((b[1] << 8) | b[0]);
  }

  uint32_t u32() {
    uint8_t b[4];
    if (!raw(b, 4)) return 0;
    if (!_big) std::swap(b[0], b[3]), std::swap(b[1], b[2]);
    return (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
           (static_cast<uint32_t>(b[2]) << 8) | static_cast<uint32_t>(b[3]);
  }

  uint64_t u64() {
    uint8_t b[8];
    if (!raw(b, 8)) return 0;
    if (!_big) for (int i = 0; i < 4; i++) std::swap(b[i], b[7 - i]);
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | b[i];
    return value;
  }

  uint32_t u32be() {
    bool const saved = _big;
    _big = true;
    uint32_t const value = u32();
    _big = saved;
    return value;
  }

  uint64_t u64be() {
    bool const saved = _big;
    _big = true;
    uint64_t const value = u64();
    _big = saved;
    return value;
  }

  // NUL-terminated string; a missing terminator within `limit` is a failure.
  std::string cstring(size_t limit) {
    std::string out;
    while (out.size() < limit) {
      if (remaining() < 1) { _ok = false; return {}; }
      char const c = static_cast<char>(_data[_pos++]);
      if (c == '\0') return out;
      out.push_back(c);
    }
    _ok = false;
    return {};
  }

  bool raw(uint8_t* dest, size_t count) {
    if (remaining() < count) { _ok = false; _pos = _size; return false; }
    std::memcpy(dest, _data + _pos, count);
    _pos += count;
    return true;
  }

 private:
  uint8_t const* _data;
  size_t _size;
  size_t _pos = 0;
  bool _ok = true;
  bool _big = false;
};

struct TypeTreeNode {
  uint16_t version = 0;
  uint8_t level = 0;
  uint8_t typeFlags = 0;
  uint32_t typeStrOffset = 0;
  uint32_t nameStrOffset = 0;
  int32_t byteSize = 0;
  int32_t index = 0;
  uint32_t metaFlag = 0;
};

struct SerializedTypeInfo {
  int32_t classID = 0;
  std::vector<TypeTreeNode> nodes;
  std::string stringBuffer;
};

struct ObjectInfo {
  int64_t pathID = 0;
  int64_t byteStart = 0;
  uint32_t byteSize = 0;
  int32_t typeIndex = 0;
  // Where this entry's byteStart and byteSize fields sit in the file. The
  // rewriter patches them in place rather than rebuilding the metadata, which
  // is what keeps the type tree, externals and everything else byte-identical.
  size_t byteStartFieldOffset = 0;
  size_t byteSizeFieldOffset = 0;
};

// Everything about a SerializedFile that both reading its shaders and rewriting
// its objects need. Parsed once, by ParseLayout.
struct Layout {
  bool isSerializedFile = false;
  bool parsed = false;
  uint32_t version = 0;
  bool bigEndian = false;
  size_t headerLength = 0;
  uint64_t metadataSize = 0;
  uint64_t fileSize = 0;
  uint64_t dataOffset = 0;
  bool typeTreePresent = false;
  std::string unityVersion;
  std::vector<SerializedTypeInfo> types;
  std::vector<ObjectInfo> objects;
  std::string message;
};

// Unity's built-in "common string" table, which type trees reference with the
// high bit set instead of repeating the name in every file. Offsets are byte
// positions in this buffer. Names like m_Index, m_Type, Array and data are
// only ever written this way, so without it a walk by name cannot tell a
// constant-buffer offset from any other int. Unity only appends to the table,
// so one copy serves every version this reads. (From UnityPy's type-tree
// package; see tools/bundleconvert/fixtures.)
constexpr char kCommonStrings[] =
    "AABB\0"
    "AnimationClip\0"
    "AnimationCurve\0"
    "AnimationState\0"
    "Array\0"
    "Base\0"
    "BitField\0"
    "bitset\0"
    "bool\0"
    "char\0"
    "ColorRGBA\0"
    "Component\0"
    "data\0"
    "deque\0"
    "double\0"
    "dynamic_array\0"
    "FastPropertyName\0"
    "first\0"
    "float\0"
    "Font\0"
    "GameObject\0"
    "Generic Mono\0"
    "GradientNEW\0"
    "GUID\0"
    "GUIStyle\0"
    "int\0"
    "list\0"
    "long long\0"
    "map\0"
    "Matrix4x4f\0"
    "MdFour\0"
    "MonoBehaviour\0"
    "MonoScript\0"
    "m_ByteSize\0"
    "m_Curve\0"
    "m_EditorClassIdentifier\0"
    "m_EditorHideFlags\0"
    "m_Enabled\0"
    "m_ExtensionPtr\0"
    "m_GameObject\0"
    "m_Index\0"
    "m_IsArray\0"
    "m_IsStatic\0"
    "m_MetaFlag\0"
    "m_Name\0"
    "m_ObjectHideFlags\0"
    "m_PrefabInternal\0"
    "m_PrefabParentObject\0"
    "m_Script\0"
    "m_StaticEditorFlags\0"
    "m_Type\0"
    "m_Version\0"
    "Object\0"
    "pair\0"
    "PPtr<Component>\0"
    "PPtr<GameObject>\0"
    "PPtr<Material>\0"
    "PPtr<MonoBehaviour>\0"
    "PPtr<MonoScript>\0"
    "PPtr<Object>\0"
    "PPtr<Prefab>\0"
    "PPtr<Sprite>\0"
    "PPtr<TextAsset>\0"
    "PPtr<Texture>\0"
    "PPtr<Texture2D>\0"
    "PPtr<Transform>\0"
    "Prefab\0"
    "Quaternionf\0"
    "Rectf\0"
    "RectInt\0"
    "RectOffset\0"
    "second\0"
    "set\0"
    "short\0"
    "size\0"
    "SInt16\0"
    "SInt32\0"
    "SInt64\0"
    "SInt8\0"
    "staticvector\0"
    "string\0"
    "TextAsset\0"
    "TextMesh\0"
    "Texture\0"
    "Texture2D\0"
    "Transform\0"
    "TypelessData\0"
    "UInt16\0"
    "UInt32\0"
    "UInt64\0"
    "UInt8\0"
    "unsigned int\0"
    "unsigned long long\0"
    "unsigned short\0"
    "vector\0"
    "Vector2f\0"
    "Vector3f\0"
    "Vector4f\0"
    "m_ScriptingClassIdentifier\0"
    "Gradient\0"
    "Type*\0"
    "int2_storage\0"
    "int3_storage\0"
    "BoundsInt\0"
    "m_CorrespondingSourceObject\0"
    "m_PrefabInstance\0"
    "m_PrefabAsset\0"
    "FileSize\0"
    "Hash128\0"
    "RenderingLayerMask\0"
    "fixed_array\0"
    "EntityId\0"
    "LoadableObjectId\0"
    "LoadableSceneId\0";

std::string_view CommonString(uint32_t offset) {
  constexpr size_t kSize = sizeof(kCommonStrings) - 1;  // the literal's own trailing NUL
  if (offset >= kSize) return {};
  char const* base = kCommonStrings + offset;
  return std::string_view(base, std::strlen(base));
}

// Resolves a node's name, through the common table when the offset has its high
// bit set.
std::string NodeName(SerializedTypeInfo const& type, TypeTreeNode const& node) {
  if ((node.nameStrOffset & kCommonStringBit) != 0) {
    return std::string(CommonString(node.nameStrOffset & ~kCommonStringBit));
  }
  if (node.nameStrOffset >= type.stringBuffer.size()) return {};
  char const* base = type.stringBuffer.data() + node.nameStrOffset;
  size_t const maxLen = type.stringBuffer.size() - node.nameStrOffset;
  size_t len = 0;
  while (len < maxLen && base[len] != '\0') len++;
  return std::string(base, len);
}

// Indices of the direct children of nodes[parent] in the flat, depth-ordered
// node array: every node deeper than the parent belongs to it, and the run ends
// at the first node back at the parent's level or shallower.
std::vector<size_t> ChildIndices(std::vector<TypeTreeNode> const& nodes, size_t parent) {
  std::vector<size_t> children;
  if (parent >= nodes.size()) return children;
  uint8_t const parentLevel = nodes[parent].level;
  for (size_t i = parent + 1; i < nodes.size(); i++) {
    if (nodes[i].level <= parentLevel) break;
    if (nodes[i].level == parentLevel + 1) children.push_back(i);
  }
  return children;
}

// What a walk of one named field should bring back.
//
// A field is described by the type tree, not by this code, so the same capture
// serves `platforms` (vector<int>, one group), `offsets` (vector<vector<int>>,
// one group per inner array) and `compressedBlob` (vector<UInt8>, a byte range).
// Nothing here needs to know which of those it is looking at.
struct FieldCapture {
  bool wantBytes = false;

  std::vector<std::vector<int32_t>> groups;  // one per int array encountered
  // Where each group's first element sits, relative to the start of the object
  // body. Conversion patches these tables in place -- the counts do not change,
  // only the values -- so the writer needs to know where they are rather than
  // having to re-serialize the whole object around them.
  std::vector<size_t> groupOffsets;
  bool sawBytes = false;
  size_t byteOffset = 0;  // relative to the start of the object body
  size_t byteCount = 0;
  // Whether the byte array is followed by alignment padding. A converted blob
  // is a different length, so the padding after it has to be recomputed, and
  // that is only correct if it is known to be there in the first place.
  bool bytesAligned = false;

  bool empty() const { return groups.empty() && !sawBytes; }
};

// Walks one field, advancing the reader past it. `capture`, when non-null,
// records this field's contents -- which is how `platforms`, the three length
// tables and the blob are all read without special-casing any of their layouts.
void WalkNode(Reader& reader, SerializedTypeInfo const& type, size_t nodeIndex,
              FieldCapture* capture, int depth) {
  if (!reader.ok() || nodeIndex >= type.nodes.size()) return;
  // Bundles are untrusted input; a cyclic or absurdly deep tree must not
  // recurse without bound.
  if (depth > 48) { reader.skip(reader.remaining()); return; }

  TypeTreeNode const& node = type.nodes[nodeIndex];
  std::vector<size_t> const children = ChildIndices(type.nodes, nodeIndex);

  if ((node.typeFlags & kTypeFlagIsArray) != 0) {
    // Children are exactly [size, data]. The size is a plain int; the element
    // layout is the data node, repeated.
    if (children.size() < 2) { reader.skip(reader.remaining()); return; }
    uint32_t const count = reader.u32();
    if (!reader.ok()) return;
    size_t const dataNode = children[1];
    TypeTreeNode const& element = type.nodes[dataNode];
    bool const elementIsFlat = element.byteSize > 0 && ChildIndices(type.nodes, dataNode).empty();

    if (elementIsFlat) {
      // A flat element array is a contiguous run; read it in one step rather
      // than looping, and refuse a count the remaining bytes cannot support.
      uint64_t const bytes = static_cast<uint64_t>(count) * static_cast<uint64_t>(element.byteSize);
      if (bytes > reader.remaining()) { reader.skip(reader.remaining()); return; }
      if (capture != nullptr && capture->wantBytes && element.byteSize == 1) {
        // The blob is recorded as a range, not copied: it is the largest thing
        // in the file by a wide margin.
        capture->sawBytes = true;
        capture->byteOffset = reader.position();
        capture->byteCount = static_cast<size_t>(bytes);
        capture->bytesAligned = (node.metaFlag & kMetaFlagAlignBytes) != 0 ||
                                (element.metaFlag & kMetaFlagAlignBytes) != 0;
        reader.skip(static_cast<size_t>(bytes));
      } else if (capture != nullptr && !capture->wantBytes && element.byteSize == 4) {
        std::vector<int32_t> group;
        group.reserve(count);
        capture->groupOffsets.push_back(reader.position());
        for (uint32_t i = 0; i < count; i++) group.push_back(static_cast<int32_t>(reader.u32()));
        capture->groups.push_back(std::move(group));
      } else {
        reader.skip(static_cast<size_t>(bytes));
      }
    } else {
      if (count > reader.remaining()) { reader.skip(reader.remaining()); return; }
      for (uint32_t i = 0; i < count && reader.ok(); i++) {
        // Every element of a nested array belongs to the same field, so the
        // capture stays armed across all of them -- that is what turns
        // vector<vector<int>> into one group per inner array.
        WalkNode(reader, type, dataNode, capture, depth + 1);
      }
    }
  } else if (children.empty()) {
    if (node.byteSize < 0) { reader.skip(reader.remaining()); return; }
    reader.skip(static_cast<size_t>(node.byteSize));
  } else {
    for (size_t child : children) {
      if (!reader.ok()) return;
      // A field like `platforms` is a *vector* node wrapping the actual Array
      // node, so the capture has to be handed down through the wrapper or it
      // would never reach the elements. A wrapper is recognisable by having
      // exactly one child, and always passes the capture on -- which is what
      // lets vector<vector<int>> keep collecting past its first inner array.
      //
      // A real struct has several children, and there the capture stops at the
      // first thing collected so unrelated sibling arrays cannot be merged.
      bool const isWrapper = children.size() == 1;
      bool const stillLooking = capture != nullptr && (isWrapper || capture->empty());
      WalkNode(reader, type, child, stillLooking ? capture : nullptr, depth + 1);
    }
  }

  if ((node.metaFlag & kMetaFlagAlignBytes) != 0) reader.align4();
}

// Walks one field like WalkNode, recording where each PPtr in it sits: a
// struct of exactly [m_FileID (4 bytes), m_PathID (8 bytes)]. Offsets are
// relative to the start of what `reader` reads.
void WalkForPPtrs(Reader& reader, SerializedTypeInfo const& type, size_t nodeIndex,
                  std::vector<size_t>& pptrs, int depth) {
  if (!reader.ok() || nodeIndex >= type.nodes.size()) return;
  if (depth > 48) { reader.skip(reader.remaining()); return; }
  TypeTreeNode const& node = type.nodes[nodeIndex];
  std::vector<size_t> const children = ChildIndices(type.nodes, nodeIndex);
  if ((node.typeFlags & kTypeFlagIsArray) != 0) {
    if (children.size() < 2) { reader.skip(reader.remaining()); return; }
    uint32_t const count = reader.u32();
    if (!reader.ok()) return;
    size_t const dataNode = children[1];
    TypeTreeNode const& element = type.nodes[dataNode];
    if (element.byteSize > 0 && ChildIndices(type.nodes, dataNode).empty()) {
      uint64_t const bytes = static_cast<uint64_t>(count) * static_cast<uint64_t>(element.byteSize);
      if (bytes > reader.remaining()) { reader.skip(reader.remaining()); return; }
      reader.skip(static_cast<size_t>(bytes));
    } else {
      if (count > reader.remaining()) { reader.skip(reader.remaining()); return; }
      for (uint32_t i = 0; i < count && reader.ok(); i++) WalkForPPtrs(reader, type, dataNode, pptrs, depth + 1);
    }
  } else if (children.empty()) {
    if (node.byteSize < 0) { reader.skip(reader.remaining()); return; }
    reader.skip(static_cast<size_t>(node.byteSize));
  } else {
    if (children.size() == 2 && type.nodes[children[0]].byteSize == 4 && type.nodes[children[1]].byteSize == 8 &&
        NodeName(type, type.nodes[children[0]]) == "m_FileID" &&
        NodeName(type, type.nodes[children[1]]) == "m_PathID") {
      pptrs.push_back(reader.position());
    }
    for (size_t child : children) {
      if (!reader.ok()) return;
      WalkForPPtrs(reader, type, child, pptrs, depth + 1);
    }
  }
  if ((node.metaFlag & kMetaFlagAlignBytes) != 0) reader.align4();
}

// ---------------------------------------------------------------------------
// Parameter records
//
// Reads a type-tree-described value into a loose record -- ints, strings and
// lists of sub-records by field name -- which is all SerializedProgramParameters
// and m_NameIndices need, whichever Unity version laid them out. Consumes
// exactly the bytes WalkNode would.
// ---------------------------------------------------------------------------

struct Record {
  std::map<std::string, int64_t> ints;
  std::map<std::string, std::string> strings;
  std::map<std::string, std::vector<Record>> lists;
};

class RecordReader {
 public:
  RecordReader(Reader& reader, std::vector<ParameterSchemaNode> const& nodes,
               std::vector<std::vector<size_t>> const& children)
      : _reader(reader), _nodes(nodes), _children(children) {}

  // Reads nodes[index] into `into` under its own field name.
  void Read(size_t index, Record& into, int depth = 0) {
    if (!_reader.ok() || index >= _nodes.size()) return;
    if (depth > 32) { _reader.skip(_reader.remaining()); return; }
    ParameterSchemaNode const& node = _nodes[index];
    auto const& kids = _children[index];
    if ((node.typeFlags & kTypeFlagIsArray) != 0) {
      ReadArray(index, into.lists[node.name], into, node.name, depth);
    } else if (kids.empty()) {
      if (node.byteSize < 0 || static_cast<size_t>(node.byteSize) > _reader.remaining()) {
        _reader.skip(_reader.remaining());
        return;
      }
      int64_t value = 0;
      switch (node.byteSize) {
        case 1: value = static_cast<int8_t>(_reader.u8()); break;
        case 2: value = static_cast<int16_t>(_reader.u16()); break;
        case 4: value = static_cast<int32_t>(_reader.u32()); break;
        case 8: value = static_cast<int64_t>(_reader.u64()); break;
        default: _reader.skip(static_cast<size_t>(node.byteSize)); break;
      }
      into.ints[node.name] = value;
    } else if (kids.size() == 1 && (_nodes[kids[0]].typeFlags & kTypeFlagIsArray) != 0) {
      // A vector, map or string: a wrapper round one Array, collected under the
      // wrapper's name.
      ReadArray(kids[0], into.lists[node.name], into, node.name, depth);
    } else {
      Record sub;
      for (size_t child : kids) {
        if (!_reader.ok()) break;
        Read(child, sub, depth + 1);
      }
      into.lists[node.name].push_back(std::move(sub));
    }
    if ((node.metaFlag & kMetaFlagAlignBytes) != 0) _reader.align4();
  }

 private:
  void ReadArray(size_t arrayIndex, std::vector<Record>& out, Record& owner, std::string const& name, int depth) {
    auto const& kids = _children[arrayIndex];
    if (kids.size() < 2) { _reader.skip(_reader.remaining()); return; }
    uint32_t const count = _reader.u32();
    if (!_reader.ok()) return;
    size_t const dataIndex = kids[1];
    ParameterSchemaNode const& element = _nodes[dataIndex];
    if (element.byteSize > 0 && _children[dataIndex].empty()) {
      uint64_t const bytes = static_cast<uint64_t>(count) * static_cast<uint64_t>(element.byteSize);
      if (bytes > _reader.remaining()) { _reader.skip(_reader.remaining()); return; }
      if (element.byteSize == 1) {
        std::string text(count, '\0');
        if (count > 0) _reader.raw(reinterpret_cast<uint8_t*>(text.data()), count);
        owner.strings[name] = std::move(text);
      } else {
        _reader.skip(static_cast<size_t>(bytes));
      }
    } else {
      if (count > _reader.remaining()) { _reader.skip(_reader.remaining()); return; }
      for (uint32_t i = 0; i < count && _reader.ok(); i++) {
        Record element;
        auto const& elementKids = _children[dataIndex];
        if (elementKids.empty()) {
          Read(dataIndex, element, depth + 1);
        } else {
          for (size_t child : elementKids) {
            if (!_reader.ok()) break;
            Read(child, element, depth + 1);
          }
          if ((_nodes[dataIndex].metaFlag & kMetaFlagAlignBytes) != 0) _reader.align4();
        }
        out.push_back(std::move(element));
      }
    }
    if ((_nodes[arrayIndex].metaFlag & kMetaFlagAlignBytes) != 0) _reader.align4();
  }

  Reader& _reader;
  std::vector<ParameterSchemaNode> const& _nodes;
  std::vector<std::vector<size_t>> const& _children;
};

std::vector<std::vector<size_t>> SchemaChildren(std::vector<ParameterSchemaNode> const& nodes) {
  std::vector<std::vector<size_t>> children(nodes.size());
  std::vector<size_t> open;
  for (size_t i = 0; i < nodes.size(); i++) {
    uint8_t const level = nodes[i].level;
    while (!open.empty() && nodes[open.back()].level >= level) open.pop_back();
    if (!open.empty() && nodes[open.back()].level + 1 == level) children[open.back()].push_back(i);
    open.push_back(i);
  }
  return children;
}

ProgramParameter ParameterFrom(Record const& record, bool matrix) {
  auto get = [&record](char const* key, int64_t fallback) {
    auto it = record.ints.find(key);
    return it == record.ints.end() ? fallback : it->second;
  };
  ProgramParameter parameter;
  parameter.nameIndex = static_cast<int32_t>(get("m_NameIndex", -1));
  parameter.index = static_cast<int32_t>(get("m_Index", 0));
  parameter.arraySize = static_cast<int32_t>(get("m_ArraySize", 0));
  parameter.type = static_cast<int32_t>(get("m_Type", 0));
  parameter.dim = static_cast<int32_t>(matrix ? get("m_RowCount", 4) : get("m_Dim", 0));
  parameter.samplerIndex = static_cast<int32_t>(get("m_SamplerIndex", -1));
  return parameter;
}

// Is `field` one of SerializedProgramParameters' own vectors?
bool IsParameterField(std::string_view field) {
  return field == "m_VectorParams" || field == "m_MatrixParams" || field == "m_TextureParams" ||
         field == "m_BufferParams" || field == "m_UAVParams" || field == "m_ConstantBuffers" ||
         field == "m_ConstantBufferBindings";
}

void AddParameterField(std::string const& field, std::vector<Record> const& list, ProgramParameters& out) {
  for (auto const& record : list) {
    if (field == "m_VectorParams") {
      out.vectors.push_back(ParameterFrom(record, false));
    } else if (field == "m_MatrixParams") {
      out.matrices.push_back(ParameterFrom(record, true));
    } else if (field == "m_TextureParams") {
      out.textures.push_back(ParameterFrom(record, false));
    } else if (field == "m_BufferParams") {
      out.buffers.push_back(ParameterFrom(record, false));
    } else if (field == "m_UAVParams") {
      out.uavs.push_back(ParameterFrom(record, false));
    } else if (field == "m_ConstantBufferBindings") {
      out.constantBufferBindings.push_back(ParameterFrom(record, false));
    } else if (field == "m_ConstantBuffers") {
      ProgramConstantBuffer buffer;
      auto name = record.ints.find("m_NameIndex");
      buffer.nameIndex = name == record.ints.end() ? -1 : static_cast<int32_t>(name->second);
      auto size = record.ints.find("m_Size");
      buffer.size = size == record.ints.end() ? 0 : static_cast<int32_t>(size->second);
      if (auto it = record.lists.find("m_VectorParams"); it != record.lists.end()) {
        for (auto const& v : it->second) buffer.vectors.push_back(ParameterFrom(v, false));
      }
      if (auto it = record.lists.find("m_MatrixParams"); it != record.lists.end()) {
        for (auto const& m : it->second) buffer.matrices.push_back(ParameterFrom(m, true));
      }
      if (auto it = record.lists.find("m_StructParams"); it != record.lists.end() && !it->second.empty()) {
        buffer.hasStructParams = true;
        auto const& first = it->second.front();
        auto get = [&first](char const* key) {
          auto found = first.ints.find(key);
          return found == first.ints.end() ? 0 : static_cast<int32_t>(found->second);
        };
        buffer.structOffset = get("m_Index");
        buffer.structSize = get("m_StructSize");
        buffer.structArraySize = get("m_ArraySize");
      }
      out.constantBuffers.push_back(std::move(buffer));
    }
  }
}

// Direct children of every node, built once per walk. ChildIndices rescans
// every descendant on each call, which is fine for the handful of top-level
// fields ReadShaderObject visits and quadratic for m_ParsedForm, where one
// SerializedPass node is re-entered for every pass of every subshader.
std::vector<std::vector<size_t>> BuildChildLists(std::vector<TypeTreeNode> const& nodes) {
  std::vector<std::vector<size_t>> children(nodes.size());
  std::vector<size_t> open;  // the chain of ancestors of the current node
  for (size_t i = 0; i < nodes.size(); i++) {
    uint8_t const level = nodes[i].level;
    while (!open.empty() && nodes[open.back()].level >= level) open.pop_back();
    if (!open.empty() && nodes[open.back()].level + 1 == level) children[open.back()].push_back(i);
    open.push_back(i);
  }
  return children;
}

std::string_view NodeNameView(SerializedTypeInfo const& type, TypeTreeNode const& node) {
  if ((node.nameStrOffset & kCommonStringBit) != 0) return CommonString(node.nameStrOffset & ~kCommonStringBit);
  if (node.nameStrOffset >= type.stringBuffer.size()) return {};
  char const* base = type.stringBuffer.data() + node.nameStrOffset;
  size_t const maxLen = type.stringBuffer.size() - node.nameStrOffset;
  size_t len = 0;
  while (len < maxLen && base[len] != '\0') len++;
  return std::string_view(base, len);
}

int32_t StageForProgramField(std::string_view name) {
  if (name == "progVertex") return kProgramStageVertex;
  if (name == "progFragment") return kProgramStageFragment;
  if (name == "progGeometry") return kProgramStageGeometry;
  if (name == "progHull") return kProgramStageHull;
  if (name == "progDomain") return kProgramStageDomain;
  if (name == "progRayTracing") return kProgramStageRayTracing;
  return -1;
}

// Walks m_ParsedForm and collects every pass's program references.
//
// The walk is driven entirely by the type tree -- exactly as WalkNode is -- and
// consumes exactly the bytes WalkNode would, so the caller's reader stays in
// step. What it looks for is found by the field names Unity gives them, which
// are shader-specific and so live in the file's own string table: m_SubShaders,
// m_Passes, progVertex..progRayTracing, m_SubPrograms / m_PlayerSubPrograms /
// m_ParameterBlobIndices, and inside a sub-program m_BlobIndex,
// m_GpuProgramType, m_ShaderHardwareTier and m_KeywordIndices. Structural
// names like "Array" and "data" come from Unity's common string table and
// resolve empty here, so nothing depends on them; array elements are told
// apart by their element index instead.
class ParsedFormWalker {
 public:
  ParsedFormWalker(Reader& reader, SerializedTypeInfo const& type, size_t fileOffset,
                   ShaderObject& shader)
      : _reader(reader), _type(type), _fileOffset(fileOffset), _shader(shader),
        _children(BuildChildLists(type.nodes)) {
    _schema.reserve(type.nodes.size());
    for (auto const& node : type.nodes) {
      _schema.push_back({node.level, node.typeFlags, node.byteSize, node.metaFlag,
                         std::string(NodeNameView(type, node))});
    }
  }

  void Walk(size_t nodeIndex) {
    Visit(nodeIndex, 1, -1);
    Finish();
  }

 private:
  enum Container : int32_t { kNone = 0, kSubPrograms = 1, kPlayerSubPrograms = 2, kParameterBlobs = 3 };

  struct Frame {
    std::string_view name;
    int32_t element = -1;
  };

  struct Context {
    int32_t subShader = -1;
    int32_t pass = -1;
    int32_t stage = -1;
    Container container = kNone;
    size_t containerFrame = 0;
    std::vector<int32_t> elements;  // element indices below the container
  };

  // Reads the current path into a Context. Returns false when the path is not
  // inside a pass's program list.
  bool CurrentContext(Context& ctx) const {
    enum { kExpectNothing, kExpectSubShader, kExpectPass, kExpectContainerElements } expect = kExpectNothing;
    for (size_t i = 0; i < _path.size(); i++) {
      Frame const& frame = _path[i];
      if (!frame.name.empty()) {
        if (frame.name == "m_SubShaders") {
          expect = kExpectSubShader;
        } else if (frame.name == "m_Passes") {
          expect = kExpectPass;
        } else if (int32_t const stage = StageForProgramField(frame.name); stage >= 0) {
          ctx.stage = stage;
          expect = kExpectNothing;
        } else if (ctx.stage >= 0 && ctx.container == kNone &&
                   (frame.name == "m_SubPrograms" || frame.name == "m_PlayerSubPrograms" ||
                    frame.name == "m_ParameterBlobIndices")) {
          ctx.container = frame.name == "m_SubPrograms"         ? kSubPrograms
                          : frame.name == "m_PlayerSubPrograms" ? kPlayerSubPrograms
                                                                : kParameterBlobs;
          ctx.containerFrame = i;
          expect = kExpectContainerElements;
        }
      }
      if (frame.element < 0) continue;
      if (expect == kExpectSubShader) {
        ctx.subShader = frame.element;
        expect = kExpectNothing;
      } else if (expect == kExpectPass) {
        ctx.pass = frame.element;
        expect = kExpectNothing;
      } else if (expect == kExpectContainerElements) {
        ctx.elements.push_back(frame.element);
      }
    }
    return ctx.subShader >= 0 && ctx.pass >= 0 && ctx.stage >= 0 && ctx.container != kNone;
  }

  // The named frames after the container, not counting the leaf itself. A
  // sub-program field is a direct member of the element struct, so there are
  // none; anything deeper (m_Parameters' own arrays) is not what is wanted.
  size_t NamedFramesBetween(Context const& ctx, size_t endExclusive) const {
    size_t named = 0;
    for (size_t i = ctx.containerFrame + 1; i < endExclusive && i < _path.size(); i++) {
      if (!_path[i].name.empty()) named++;
    }
    return named;
  }

  ParsedProgramRef* RefFor(Context const& ctx) {
    bool const player = ctx.container == kPlayerSubPrograms;
    size_t const wanted = player ? 2u : 1u;
    if (ctx.elements.size() != wanted) return nullptr;
    int32_t const list = player ? ctx.elements[0] : 0;
    int32_t const index = player ? ctx.elements[1] : ctx.elements[0];
    Key const key{ctx.subShader, ctx.pass, ctx.stage, player, list, index};
    auto it = _refIndex.find(key);
    if (it != _refIndex.end()) return &_shader.programRefs[it->second];
    ParsedProgramRef ref;
    ref.subShader = ctx.subShader;
    ref.pass = ctx.pass;
    ref.stage = ctx.stage;
    ref.player = player;
    ref.list = list;
    ref.index = index;
    _shader.programRefs.push_back(std::move(ref));
    _refIndex.emplace(key, _shader.programRefs.size() - 1);
    return &_shader.programRefs.back();
  }

  void OnScalar(std::string_view name, size_t position, int32_t byteSize) {
    if (name != "m_BlobIndex" && name != "m_GpuProgramType" && name != "m_ShaderHardwareTier") return;
    Context ctx;
    if (!CurrentContext(ctx) || ctx.container == kParameterBlobs) return;
    if (NamedFramesBetween(ctx, _path.size() - 1) != 0) return;
    ParsedProgramRef* ref = RefFor(ctx);
    if (ref == nullptr) return;
    uint8_t const* at = _data() + position;
    if (name == "m_BlobIndex" && byteSize == 4) {
      ref->blobIndex = static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
                       (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
      ref->blobIndexFileOffset = _fileOffset + position;
    } else if (name == "m_GpuProgramType" && byteSize == 1) {
      ref->gpuProgramType = static_cast<int8_t>(at[0]);
      ref->gpuProgramTypeFileOffset = _fileOffset + position;
    } else if (name == "m_ShaderHardwareTier" && byteSize == 1) {
      ref->hardwareTier = static_cast<int8_t>(at[0]);
    }
  }

  void OnFlatArray(size_t position, uint32_t count, int32_t elementSize) {
    // The flat array's own frame is the Array node (unnamed); its owner is the
    // named frame before it.
    std::string_view owner;
    size_t ownerFrame = 0;
    for (size_t i = _path.size(); i-- > 0;) {
      if (!_path[i].name.empty()) {
        owner = _path[i].name;
        ownerFrame = i;
        break;
      }
    }
    uint8_t const* at = _data() + position;

    if (owner == "m_KeywordIndices" && elementSize == 2) {
      Context ctx;
      if (!CurrentContext(ctx) || ctx.container == kParameterBlobs) return;
      if (NamedFramesBetween(ctx, ownerFrame) != 0) return;
      ParsedProgramRef* ref = RefFor(ctx);
      if (ref == nullptr) return;
      ref->keywordIndices.clear();
      for (uint32_t i = 0; i < count; i++) {
        ref->keywordIndices.push_back(static_cast<uint16_t>(at[i * 2] | (at[i * 2 + 1] << 8)));
      }
      return;
    }

    if (elementSize == 4) {
      // m_ParameterBlobIndices is vector<vector<uint>>: one flat inner array per
      // player sub-program list, one value per sub-program.
      Context ctx;
      if (!CurrentContext(ctx) || ctx.container != kParameterBlobs) return;
      if (ctx.elements.size() != 1 || NamedFramesBetween(ctx, _path.size()) != 0) return;
      int32_t const list = ctx.elements[0];
      for (uint32_t i = 0; i < count; i++) {
        uint32_t const value = static_cast<uint32_t>(at[i * 4]) | (static_cast<uint32_t>(at[i * 4 + 1]) << 8) |
                               (static_cast<uint32_t>(at[i * 4 + 2]) << 16) |
                               (static_cast<uint32_t>(at[i * 4 + 3]) << 24);
        _parameterBlobs.push_back({ctx.subShader, ctx.pass, ctx.stage, list, static_cast<int32_t>(i), value});
      }
      return;
    }

    if (elementSize == 1) {
      // m_ParsedForm.m_Name: the shader's real name. The Shader object's own
      // m_Name is empty in bundles built by recent Unity, so without this
      // every shader went nameless. Only the one directly under m_ParsedForm
      // (a pass, a property and a subshader tag all have an m_Name too).
      {
        std::vector<std::string_view> named;
        for (auto const& frame : _path) {
          if (!frame.name.empty()) named.push_back(frame.name);
        }
        if (named.size() == 2 && named[0] == "m_ParsedForm" && named[1] == "m_Name") {
          std::string candidate(reinterpret_cast<char const*>(at), count);
          bool printable = !candidate.empty();
          for (char c : candidate) {
            if (c < 0x20 || c > 0x7e) printable = false;
          }
          if (printable) _shader.parsedFormName = candidate;
          return;
        }
      }
      // m_KeywordNames is vector<string>, and a string is a flat char array.
      // Only the one directly under m_ParsedForm is wanted: [m_KeywordNames,
      // Array, data(i), Array] with no named frame in between.
      for (size_t i = _path.size(); i-- > 0;) {
        if (_path[i].name.empty()) continue;
        if (_path[i].name != "m_KeywordNames") return;
        // One element frame (the string) between the owner and here.
        int32_t element = -1;
        for (size_t j = i + 1; j < _path.size(); j++) {
          if (_path[j].element >= 0) element = _path[j].element;
        }
        if (element < 0) return;
        if (_shader.keywordNames.size() <= static_cast<size_t>(element)) {
          _shader.keywordNames.resize(static_cast<size_t>(element) + 1);
        }
        _shader.keywordNames[static_cast<size_t>(element)].assign(reinterpret_cast<char const*>(at), count);
        if (_shader.keywordNameFileOffsets.size() <= static_cast<size_t>(element)) {
          _shader.keywordNameFileOffsets.resize(static_cast<size_t>(element) + 1, 0);
        }
        // The string's length field sits just before its characters.
        _shader.keywordNameFileOffsets[static_cast<size_t>(element)] = _fileOffset + position - 4;
        return;
      }
    }
  }

  void Visit(size_t nodeIndex, int depth, int32_t element) {
    if (!_reader.ok() || nodeIndex >= _type.nodes.size()) return;
    if (depth > 64) { _reader.skip(_reader.remaining()); return; }
    TypeTreeNode const& node = _type.nodes[nodeIndex];
    std::vector<size_t> const& children = _children[nodeIndex];
    std::string_view frameName = NodeNameView(_type, node);
    // Structural names say nothing about which field this is; element indices
    // do that job, and treating these as named would break the "no named frame
    // between the container and the field" checks below.
    if (frameName == "Array" || frameName == "data") frameName = {};
    _path.push_back({frameName, element});

    if (Intercept(nodeIndex, frameName)) {
      _path.pop_back();
      return;
    }

    if ((node.typeFlags & kTypeFlagIsArray) != 0) {
      if (children.size() < 2) {
        _reader.skip(_reader.remaining());
      } else {
        uint32_t const count = _reader.u32();
        size_t const dataNode = children[1];
        TypeTreeNode const& elementNode = _type.nodes[dataNode];
        bool const flat = elementNode.byteSize > 0 && _children[dataNode].empty();
        if (!_reader.ok()) {
          // nothing
        } else if (flat) {
          uint64_t const bytes = static_cast<uint64_t>(count) * static_cast<uint64_t>(elementNode.byteSize);
          if (bytes > _reader.remaining()) {
            _reader.skip(_reader.remaining());
          } else {
            OnFlatArray(_reader.position(), count, elementNode.byteSize);
            _reader.skip(static_cast<size_t>(bytes));
          }
        } else if (count > _reader.remaining()) {
          _reader.skip(_reader.remaining());
        } else {
          for (uint32_t i = 0; i < count && _reader.ok(); i++) {
            Visit(dataNode, depth + 1, static_cast<int32_t>(i));
          }
        }
      }
    } else if (children.empty()) {
      if (node.byteSize < 0) {
        _reader.skip(_reader.remaining());
      } else {
        if (static_cast<size_t>(node.byteSize) <= _reader.remaining()) {
          OnScalar(_path.back().name, _reader.position(), node.byteSize);
        }
        _reader.skip(static_cast<size_t>(node.byteSize));
      }
    } else {
      for (size_t child : children) {
        if (!_reader.ok()) break;
        Visit(child, depth + 1, -1);
      }
    }

    if ((node.metaFlag & kMetaFlagAlignBytes) != 0) _reader.align4();
    _path.pop_back();
  }

  // The (subShader, pass) the path is in, whatever else it is inside.
  bool PassOf(int32_t& subShader, int32_t& pass) const {
    subShader = pass = -1;
    int expect = 0;
    for (auto const& frame : _path) {
      if (frame.name == "m_SubShaders") expect = 1;
      else if (frame.name == "m_Passes") expect = 2;
      if (frame.element < 0) continue;
      if (expect == 1) { subShader = frame.element; expect = 0; }
      else if (expect == 2) { pass = frame.element; expect = 0; }
    }
    return subShader >= 0 && pass >= 0;
  }

  // Copies the SerializedProgramParameters subtree rooted at `index` into the
  // shader's schema, levels rebased so the root is level 0.
  void CaptureSchema(size_t index) {
    if (!_shader.parameterSchema.empty()) return;
    uint8_t const base = _type.nodes[index].level;
    _shader.parameterSchema.push_back(_schema[index]);
    _shader.parameterSchema.back().level = 0;
    for (size_t i = index + 1; i < _type.nodes.size() && _type.nodes[i].level > base; i++) {
      _shader.parameterSchema.push_back(_schema[i]);
      _shader.parameterSchema.back().level = static_cast<uint8_t>(_type.nodes[i].level - base);
    }
  }

  // m_NameIndices is map<string, int>. Walks its bytes [start, end) as
  // aligned keys -- count, then (length, characters, padding to 4, value) per
  // entry -- and, only if that lands exactly on `end`, records where each
  // STEREO_INSTANCING_ON key's length field sits, so a converter can rename it
  // in place (a 2019 bundle picks its variants through these names).
  void FindAlignedNameKeys(size_t start, size_t end) {
    if (end <= start || end - start < 4) return;
    uint8_t const* const base = _data();
    auto u32 = [base](size_t at) {
      return static_cast<uint32_t>(base[at]) | (static_cast<uint32_t>(base[at + 1]) << 8) |
             (static_cast<uint32_t>(base[at + 2]) << 16) | (static_cast<uint32_t>(base[at + 3]) << 24);
    };
    uint32_t const count = u32(start);
    size_t at = start + 4;
    std::vector<size_t> found;
    for (uint32_t i = 0; i < count; i++) {
      if (end - at < 4) return;
      uint32_t const length = u32(at);
      if (length > end - at - 4) return;
      if (std::string_view(reinterpret_cast<char const*>(base + at + 4), length) == "STEREO_INSTANCING_ON") {
        found.push_back(at);
      }
      at += 4 + length;
      at = (at + 3) & ~static_cast<size_t>(3);
      if (at > end || end - at < 4) return;
      at += 4;  // the int value
    }
    if (at != end) return;
    for (size_t offset : found) _shader.stereoNameIndexFileOffsets.push_back(_fileOffset + offset);
  }

  // Reads the fields that need whole-value decoding -- parameters and the
  // pass's name table -- instead of walking them leaf by leaf. Returns true
  // when it consumed the node.
  bool Intercept(size_t nodeIndex, std::string_view name) {
    if (name == "m_CommonParameters") CaptureSchema(nodeIndex);

    if (name == "m_NameIndices") {
      int32_t subShader, pass;
      if (!PassOf(subShader, pass)) return false;
      Record record;
      size_t const start = _reader.position();
      RecordReader(_reader, _schema, _children).Read(nodeIndex, record);
      FindAlignedNameKeys(start, _reader.position());
      auto& names = _shader.passNames[{subShader, pass}];
      if (auto it = record.lists.find("m_NameIndices"); it != record.lists.end()) {
        for (auto const& pair : it->second) {
          auto first = pair.strings.find("first");
          auto second = pair.ints.find("second");
          if (first != pair.strings.end() && second != pair.ints.end()) {
            names[static_cast<int32_t>(second->second)] = first->second;
          }
        }
      }
      return true;
    }

    if (!IsParameterField(name)) return false;
    Context ctx;
    ProgramParameters* target = nullptr;
    // m_CommonParameters of a stage, or a sub-program's own (directly in the
    // element up to 2020.3, under m_Parameters from then on).
    bool inCommon = false;
    for (auto const& frame : _path) {
      if (frame.name == "m_CommonParameters") inCommon = true;
    }
    int32_t subShader, pass;
    if (inCommon) {
      int32_t stage = -1;
      for (auto const& frame : _path) {
        if (int32_t const s = StageForProgramField(frame.name); s >= 0) stage = s;
      }
      if (!PassOf(subShader, pass) || stage < 0) return false;
      target = &_common[{subShader, pass, stage}];
    } else if (CurrentContext(ctx) && ctx.container == kSubPrograms && ctx.elements.size() == 1) {
      // Only m_Parameters may sit between the element and this field.
      for (size_t i = ctx.containerFrame + 1; i + 1 < _path.size(); i++) {
        if (!_path[i].name.empty() && _path[i].name != "m_Parameters") return false;
      }
      target = &RefFor(ctx)->parameters;
    } else {
      return false;
    }
    Record record;
    RecordReader(_reader, _schema, _children).Read(nodeIndex, record);
    if (auto it = record.lists.find(std::string(name)); it != record.lists.end()) {
      AddParameterField(std::string(name), it->second, *target);
    }
    return true;
  }

  // Joins m_ParameterBlobIndices onto the player sub-programs they belong to:
  // the two are parallel arrays, [list][index] for [list][index].
  void Finish() {
    for (auto const& blob : _parameterBlobs) {
      Key const key{blob.subShader, blob.pass, blob.stage, true, blob.list, blob.index};
      auto it = _refIndex.find(key);
      if (it == _refIndex.end()) continue;
      _shader.programRefs[it->second].hasParameterBlob = true;
      _shader.programRefs[it->second].parameterBlobIndex = blob.value;
    }
    // A reference is only useful if both of the fields conversion rewrites were
    // actually located; anything else means the tree was not the shape this
    // walk understands, and is dropped rather than half-trusted.
    std::vector<ParsedProgramRef> kept;
    kept.reserve(_shader.programRefs.size());
    for (auto& ref : _shader.programRefs) {
      if (ref.blobIndexFileOffset == 0 || ref.gpuProgramTypeFileOffset == 0) continue;
      if (auto common = _common.find({ref.subShader, ref.pass, ref.stage}); common != _common.end()) {
        ref.parameters.Merge(common->second);
      }
      if (auto names = _shader.passNames.find({ref.subShader, ref.pass}); names != _shader.passNames.end()) {
        ref.parameters.ResolveNames(names->second);
      }
      kept.push_back(std::move(ref));
    }
    _shader.programRefs = std::move(kept);
    _shader.parsedFormRead = !_shader.programRefs.empty();
  }

  uint8_t const* _data() const { return _reader.base(); }

  struct Key {
    int32_t subShader, pass, stage;
    bool player;
    int32_t list, index;
    bool operator<(Key const& other) const {
      return std::tie(subShader, pass, stage, player, list, index) <
             std::tie(other.subShader, other.pass, other.stage, other.player, other.list, other.index);
    }
  };
  struct ParameterBlob {
    int32_t subShader, pass, stage, list, index;
    uint32_t value;
  };

  Reader& _reader;
  SerializedTypeInfo const& _type;
  size_t _fileOffset;
  ShaderObject& _shader;
  std::vector<std::vector<size_t>> _children;
  std::vector<Frame> _path;
  std::map<Key, size_t> _refIndex;
  std::vector<ParameterBlob> _parameterBlobs;
  std::vector<ParameterSchemaNode> _schema;  // the whole type, names resolved
  std::map<std::tuple<int32_t, int32_t, int32_t>, ProgramParameters> _common;
};

// Reads a Shader object: walks its top-level fields in order, capturing the one
// named "platforms" and the shader's name if it is reachable by name.
ShaderObject ReadShaderObject(uint8_t const* data, size_t size, size_t fileOffset,
                              SerializedTypeInfo const& type) {
  ShaderObject shader;
  if (type.nodes.empty()) return shader;

  // The five fields that describe the compiled program store. Everything else
  // in a Shader object is walked only to stay in step with the byte stream.
  auto captureInts = [&](Reader& reader, size_t child, std::vector<std::vector<int32_t>>& into,
                         std::vector<size_t>& locations) {
    FieldCapture capture;
    WalkNode(reader, type, child, &capture, 1);
    into = std::move(capture.groups);
    locations.clear();
    for (size_t offset : capture.groupOffsets) locations.push_back(fileOffset + offset);
  };

  shader.bodyFileOffset = fileOffset;
  shader.bodySize = size;
  Reader reader(data, size);
  for (size_t child : ChildIndices(type.nodes, 0)) {
    if (!reader.ok()) break;
    std::string const name = NodeName(type, type.nodes[child]);
    if (name == "platforms") {
      FieldCapture capture;
      WalkNode(reader, type, child, &capture, 1);
      // platforms is flat, so there is exactly one group; older bundles that
      // nest it would give several, and concatenating them is still correct.
      for (auto const& group : capture.groups) {
        shader.platforms.insert(shader.platforms.end(), group.begin(), group.end());
      }
      for (size_t offset : capture.groupOffsets) {
        shader.platformsTableOffsets.push_back(fileOffset + offset);
      }
    } else if (name == "offsets") {
      captureInts(reader, child, shader.offsets, shader.offsetsTableOffsets);
    } else if (name == "compressedLengths") {
      captureInts(reader, child, shader.compressedLengths, shader.compressedLengthsTableOffsets);
    } else if (name == "decompressedLengths") {
      captureInts(reader, child, shader.decompressedLengths,
                  shader.decompressedLengthsTableOffsets);
    } else if (name == "compressedBlob") {
      FieldCapture capture;
      capture.wantBytes = true;
      WalkNode(reader, type, child, &capture, 1);
      if (capture.sawBytes) {
        shader.blobPresent = true;
        shader.blobFileOffset = fileOffset + capture.byteOffset;
        shader.blobSize = capture.byteCount;
        shader.blobAligned = capture.bytesAligned;
      }
    } else {
      size_t const before = reader.position();
      if (name == "m_ParsedForm") {
        ParsedFormWalker walker(reader, type, fileOffset, shader);
        walker.Walk(child);
      } else {
        std::vector<size_t> pptrs;
        WalkForPPtrs(reader, type, child, pptrs, 1);
        for (size_t offset : pptrs) shader.pptrFileOffsets.push_back(fileOffset + offset);
      }
      // A real Shader object starts with its NamedObject m_Name, whose field
      // name comes from Unity's common string table and so resolves empty
      // here. It is the only unnamed variable-size field before m_ParsedForm.
      if (shader.name.empty() && (name.empty() || name == "m_Name") &&
          child == ChildIndices(type.nodes, 0).front() &&
          type.nodes[child].byteSize < 0 && reader.position() >= before + 4) {
        Reader nameReader(data + before, reader.position() - before);
        uint32_t const length = nameReader.u32();
        if (nameReader.ok() && length > 0 && length <= 256 && length <= nameReader.remaining()) {
          std::string candidate;
          bool printable = true;
          for (uint32_t i = 0; i < length; i++) {
            char const c = static_cast<char>(nameReader.u8());
            if (c < 0x20 || c > 0x7e) { printable = false; break; }
            candidate.push_back(c);
          }
          if (printable) shader.name = candidate;
        }
      }
      // m_ParsedForm carries the shader's name. Rather than decode that whole
      // nested structure, take the first NUL-free run of printable bytes at its
      // start, which is where the serialized m_Name string lands.
      if (shader.name.empty() && name == "m_ParsedForm" && reader.position() > before) {
        Reader nameReader(data + before, std::min<size_t>(reader.position() - before, 512));
        uint32_t const length = nameReader.u32();
        if (nameReader.ok() && length > 0 && length <= 256 && length <= nameReader.remaining()) {
          std::string candidate;
          bool printable = true;
          for (uint32_t i = 0; i < length; i++) {
            char const c = static_cast<char>(nameReader.u8());
            if (c < 0x20 || c > 0x7e) { printable = false; break; }
            candidate.push_back(c);
          }
          if (printable && nameReader.ok()) shader.name = candidate;
        }
      }
    }
  }
  // The parsed form's own name is the authoritative one; prefer it.
  if (!shader.parsedFormName.empty()) shader.name = shader.parsedFormName;
  return shader;
}

// Reads a Texture2D object: walks its top-level fields in order, keeping the
// handful that decide whether its pixels can be reached on device.
//
// Nothing here is positional. Unity has moved Texture2D's fields around several
// times -- m_MipsStripped, m_IsAlphaChannelOptional and m_IgnoreMipmapLimit all
// arrived in different versions -- so every field is found by the name the
// file's own type tree gives it, and anything unrecognised is walked past to
// stay in step with the byte stream.
TextureObject ReadTextureObject(uint8_t const* data, size_t size, size_t fileOffset,
                                SerializedTypeInfo const& type) {
  TextureObject texture;
  if (type.nodes.empty()) return texture;
  texture.bodyFileOffset = fileOffset;
  texture.bodySize = size;

  auto readI32 = [&](size_t at) -> int32_t {
    if (at + 4 > size) return 0;
    uint32_t value = 0;
    std::memcpy(&value, data + at, 4);
    return static_cast<int32_t>(value);
  };

  Reader reader(data, size);
  for (size_t child : ChildIndices(type.nodes, 0)) {
    if (!reader.ok()) break;
    std::string const name = NodeName(type, type.nodes[child]);
    size_t const before = reader.position();

    if (name == "m_StreamData") {
      // Walked child by child rather than in one step, because which of them is
      // "size" and which is "path" is the whole question: a texture whose
      // pixels live in a companion .resS node has nothing inline to decode.
      for (size_t sub : ChildIndices(type.nodes, child)) {
        if (!reader.ok()) break;
        std::string const subName = NodeName(type, type.nodes[sub]);
        size_t const subBefore = reader.position();
        WalkNode(reader, type, sub, nullptr, 2);
        if (reader.position() <= subBefore) continue;
        if (subName == "size") {
          texture.streamDataSize = static_cast<uint32_t>(readI32(subBefore));
        } else if (subName == "path") {
          // A string is a length-prefixed byte array; a non-empty one means the
          // pixels are somewhere else.
          if (readI32(subBefore) > 0) texture.streamed = true;
        }
      }
      // WalkNode would have applied this after its children; walking them by
      // hand means applying it by hand.
      if ((type.nodes[child].metaFlag & kMetaFlagAlignBytes) != 0) reader.align4();
      continue;
    }

    if (name == "image data") {
      FieldCapture capture;
      capture.wantBytes = true;
      WalkNode(reader, type, child, &capture, 1);
      if (capture.sawBytes) {
        texture.imageDataFileOffset = fileOffset + capture.byteOffset;
        texture.imageDataSize = capture.byteCount;
      }
      continue;
    }

    WalkNode(reader, type, child, nullptr, 1);
    if (reader.position() <= before) continue;

    if (name == "m_Name") {
      uint32_t const length = static_cast<uint32_t>(readI32(before));
      if (length > 0 && length <= 256 && before + 4 + length <= size) {
        texture.name.assign(reinterpret_cast<char const*>(data + before + 4), length);
      }
    } else if (name == "m_Width") {
      texture.width = readI32(before);
    } else if (name == "m_Height") {
      texture.height = readI32(before);
    } else if (name == "m_MipCount") {
      texture.mipCount = readI32(before);
    } else if (name == "m_TextureFormat") {
      texture.textureFormat = readI32(before);
    } else if (name == "m_CompleteImageSize") {
      texture.completeImageSize = readI32(before);
    } else if (name == "m_IsReadable") {
      // Only a genuine one-byte bool is claimed. A field of any other width
      // under this name is something this code does not understand, and writing
      // to it would corrupt the object.
      if (type.nodes[child].byteSize == 1 && before < size) {
        texture.isReadablePresent = true;
        texture.isReadableFileOffset = fileOffset + before;
        texture.isReadable = data[before] != 0;
      }
    }
  }
  return texture;
}

}  // namespace

bool TextureFormatNeedsDecodingOnQuest(int32_t unityTextureFormat) {
  switch (unityTextureFormat) {
    case 10:  // DXT1 / BC1
    case 11:  // DXT3
    case 12:  // DXT5 / BC3
    case 24:  // BC6H
    case 25:  // BC7
    case 26:  // BC4
    case 27:  // BC5
      return true;
    default:
      return false;
  }
}

std::string_view ShaderPlatformName(int32_t platform) {
  switch (platform) {
    case kShaderPlatformGL: return "OpenGL";
    case kShaderPlatformD3D9: return "Direct3D 9";
    case 2: return "Xbox360";
    case 3: return "PS3";
    case kShaderPlatformD3D11: return "Direct3D 11";
    case kShaderPlatformGLES20: return "OpenGL ES 2.0";
    case 8: return "Direct3D 11 9.x";
    case kShaderPlatformGLES3Plus: return "OpenGL ES 3.x";
    case 11: return "PS4";
    case 12: return "Xbox One";
    case kShaderPlatformMetal: return "Metal";
    case kShaderPlatformOpenGLCore: return "OpenGL Core";
    case kShaderPlatformVulkan: return "Vulkan";
    case 19: return "Switch";
    case 23: return "PS5";
    default: return "unknown";
  }
}

std::string_view GpuProgramTypeName(int32_t programType) {
  switch (programType) {
    case kGpuProgramGLLegacy: return "GL legacy";
    case kGpuProgramGLES31AEP: return "GLES 3.1 AEP";
    case kGpuProgramGLES31: return "GLES 3.1";
    case kGpuProgramGLES3: return "GLES 3.0";
    case kGpuProgramGLES: return "GLES 2.0";
    case kGpuProgramGLCore32: return "GL core 3.2";
    case kGpuProgramGLCore41: return "GL core 4.1";
    case kGpuProgramGLCore43: return "GL core 4.3";
    case kGpuProgramDX11VertexSM40: return "D3D11 vertex sm4.0";
    case kGpuProgramDX11VertexSM50: return "D3D11 vertex sm5.0";
    case kGpuProgramDX11PixelSM40: return "D3D11 pixel sm4.0";
    case kGpuProgramDX11PixelSM50: return "D3D11 pixel sm5.0";
    case kGpuProgramDX11GeometrySM40: return "D3D11 geometry sm4.0";
    case kGpuProgramDX11GeometrySM50: return "D3D11 geometry sm5.0";
    case kGpuProgramDX11HullSM50: return "D3D11 hull sm5.0";
    case kGpuProgramDX11DomainSM50: return "D3D11 domain sm5.0";
    case kGpuProgramMetalVS: return "Metal vertex";
    case kGpuProgramMetalFS: return "Metal fragment";
    case kGpuProgramSPIRV: return "SPIR-V";
    default: return "unknown";
  }
}

bool GpuProgramIsGlslSource(int32_t programType) {
  switch (programType) {
    case kGpuProgramGLLegacy:
    case kGpuProgramGLES31AEP:
    case kGpuProgramGLES31:
    case kGpuProgramGLES3:
    case kGpuProgramGLES:
    case kGpuProgramGLCore32:
    case kGpuProgramGLCore41:
    case kGpuProgramGLCore43:
      return true;
    default:
      return false;
  }
}

size_t Lz4CompressBound(size_t srcSize) {
  // One token plus a length extension byte per 255, plus the literals.
  return srcSize + srcSize / 255 + 16;
}

// LZ4 block format, compression.
//
// A single hash table of recent positions, one probe per position: the same
// shape as LZ4's "fast" mode. Shader blobs are GLSL text and DXBC, both of
// which this finds plenty of matches in, and conversion happens once per map on
// a device that would rather not spend a minute searching for a better one.
//
// Every write is bounds-checked and the function gives up rather than
// overrunning, because the caller sizes the buffer from Lz4CompressBound and a
// mistake there must not become a heap overflow on a headset.
size_t Lz4CompressBlock(uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
  if (src == nullptr || dst == nullptr || srcSize == 0) return 0;

  constexpr size_t kMinMatch = 4;
  constexpr size_t kLastLiterals = 5;
  // The format forbids a match starting inside the last 12 bytes.
  constexpr size_t kMatchFindLimit = 12;
  constexpr uint32_t kEmpty = 0xffffffffu;
  constexpr int kHashBits = 12;

  auto read32 = [src](size_t at) {
    return static_cast<uint32_t>(src[at]) | (static_cast<uint32_t>(src[at + 1]) << 8) |
           (static_cast<uint32_t>(src[at + 2]) << 16) | (static_cast<uint32_t>(src[at + 3]) << 24);
  };
  auto hash = [](uint32_t value) { return (value * 2654435761u) >> (32 - kHashBits); };

  size_t op = 0;
  size_t anchor = 0;

  // Writes a token's length nibble plus any extension bytes. Returns false if
  // the output buffer is full.
  auto writeLength = [&](size_t length, uint8_t& nibble) {
    if (length < 15) {
      nibble = static_cast<uint8_t>(length);
      return true;
    }
    nibble = 15;
    size_t rest = length - 15;
    while (rest >= 255) {
      if (op >= dstCapacity) return false;
      dst[op++] = 255;
      rest -= 255;
    }
    if (op >= dstCapacity) return false;
    dst[op++] = static_cast<uint8_t>(rest);
    return true;
  };

  size_t const matchLimit = srcSize > kLastLiterals ? srcSize - kLastLiterals : 0;
  size_t const findLimit = srcSize > kMatchFindLimit ? srcSize - kMatchFindLimit : 0;

  if (findLimit > 0) {
    std::vector<uint32_t> table(static_cast<size_t>(1) << kHashBits, kEmpty);
    table[hash(read32(0))] = 0;
    size_t ip = 1;
    while (ip < findLimit) {
      uint32_t const h = hash(read32(ip));
      uint32_t const candidate = table[h];
      table[h] = static_cast<uint32_t>(ip);
      if (candidate == kEmpty || ip - candidate > 65535 || read32(candidate) != read32(ip)) {
        ip++;
        continue;
      }

      size_t matchLength = kMinMatch;
      while (ip + matchLength < matchLimit && src[candidate + matchLength] == src[ip + matchLength]) {
        matchLength++;
      }

      size_t const literalLength = ip - anchor;
      size_t const tokenPos = op;
      if (op >= dstCapacity) return 0;
      op++;

      uint8_t literalNibble = 0;
      if (!writeLength(literalLength, literalNibble)) return 0;
      if (literalLength > dstCapacity - op) return 0;
      std::memcpy(dst + op, src + anchor, literalLength);
      op += literalLength;

      size_t const offset = ip - candidate;
      if (dstCapacity - op < 2) return 0;
      dst[op++] = static_cast<uint8_t>(offset & 0xff);
      dst[op++] = static_cast<uint8_t>(offset >> 8);

      uint8_t matchNibble = 0;
      if (!writeLength(matchLength - kMinMatch, matchNibble)) return 0;
      dst[tokenPos] = static_cast<uint8_t>((literalNibble << 4) | matchNibble);

      ip += matchLength;
      anchor = ip;
      // Seeding one position back picks up matches that start mid-run, which
      // costs nothing and is what LZ4 itself does.
      if (ip >= 2 && ip < findLimit) table[hash(read32(ip - 2))] = static_cast<uint32_t>(ip - 2);
    }
  }

  // The block always ends with a literals-only sequence: the format requires
  // the last five bytes to be literals, and a trailing match has nowhere to
  // put its token.
  size_t const literalLength = srcSize - anchor;
  size_t const tokenPos = op;
  if (op >= dstCapacity) return 0;
  op++;
  uint8_t literalNibble = 0;
  if (!writeLength(literalLength, literalNibble)) return 0;
  if (literalLength > dstCapacity - op) return 0;
  std::memcpy(dst + op, src + anchor, literalLength);
  op += literalLength;
  dst[tokenPos] = static_cast<uint8_t>(literalNibble << 4);
  return op;
}

// LZ4 block format, decompression only.
//
// A block is a sequence of: one token byte, high nibble = literal length, low
// nibble = match length - 4; optional length-extension bytes for either nibble
// (0xf means "add the following bytes until one is not 0xff"); the literals
// themselves; then a 2-byte little-endian offset back into the output followed
// by the match. The final sequence ends after its literals with no match.
//
// Written out here rather than pulled in as a dependency: it is small, it has
// to run inside a Beat Saber mod on a headset, and it is fed untrusted bundle
// bytes, so every read and write is bounds-checked against the two buffers.
size_t Lz4DecodeBlock(uint8_t const* src, size_t srcSize, uint8_t* dst, size_t dstCapacity) {
  if (src == nullptr || dst == nullptr) return 0;
  size_t sp = 0;
  size_t dp = 0;

  while (sp < srcSize) {
    uint8_t const token = src[sp++];

    size_t literalLength = token >> 4;
    if (literalLength == 15) {
      while (true) {
        if (sp >= srcSize) return 0;
        uint8_t const add = src[sp++];
        literalLength += add;
        if (add != 0xff) break;
        // A run of 0xff bytes could otherwise be used to overflow the length.
        if (literalLength > dstCapacity) return 0;
      }
    }
    if (literalLength > srcSize - sp) return 0;
    if (literalLength > dstCapacity - dp) return 0;
    std::memcpy(dst + dp, src + sp, literalLength);
    sp += literalLength;
    dp += literalLength;

    // The last sequence stops here: no offset follows its literals.
    if (sp == srcSize) break;
    if (srcSize - sp < 2) return 0;
    size_t const offset = static_cast<size_t>(src[sp]) | (static_cast<size_t>(src[sp + 1]) << 8);
    sp += 2;
    if (offset == 0 || offset > dp) return 0;

    size_t matchLength = static_cast<size_t>(token & 0x0f);
    if (matchLength == 15) {
      while (true) {
        if (sp >= srcSize) return 0;
        uint8_t const add = src[sp++];
        matchLength += add;
        if (add != 0xff) break;
        if (matchLength > dstCapacity) return 0;
      }
    }
    matchLength += 4;
    if (matchLength > dstCapacity - dp) return 0;

    // Overlapping matches are legal and common (offset 1 means "repeat the last
    // byte"), so this copies forward one byte at a time rather than memmove.
    size_t from = dp - offset;
    for (size_t i = 0; i < matchLength; i++) dst[dp++] = dst[from++];
  }
  return dp;
}

namespace {

// Parses one compiled sub-program's header.
//
// Layout, as Unity writes it: format version, ShaderGpuProgramType, three ints
// of statistics, a fourth int from 2016.08 onwards, then the keyword table as
// aligned strings, then the program bytes as a byte array.
bool ReadSubProgram(uint8_t const* data, size_t size, ShaderSubProgram& out) {
  Reader reader(data, size);
  out.blobVersion = static_cast<int32_t>(reader.u32());
  out.programType = static_cast<int32_t>(reader.u32());

  // The statistics block is stepped over rather than interpreted, but it is
  // kept: a re-encoded program has to be the one Unity wrote, not a nearly
  // identical one, or a conversion bug and a re-encoding bug look the same.
  size_t const statsSize = out.blobVersion >= 201608170 ? 16u : 12u;
  if (statsSize > reader.remaining()) return false;
  out.stats.resize(statsSize);
  if (!reader.raw(out.stats.data(), statsSize)) return false;

  auto readKeywords = [&reader](std::vector<std::string>& into) {
    uint32_t const count = reader.u32();
    if (!reader.ok() || count > reader.remaining() / 4) return false;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t const length = reader.u32();
      if (!reader.ok() || length > reader.remaining()) return false;
      std::string keyword(length, '\0');
      if (length > 0 && !reader.raw(reinterpret_cast<uint8_t*>(keyword.data()), length)) return false;
      reader.align4();
      into.push_back(std::move(keyword));
    }
    return true;
  };

  if (!readKeywords(out.keywords)) return false;
  // 2018.06 through 2020.12 wrote a second, local keyword table in the same
  // shape immediately after the first. It is kept separate from the first so
  // re-encoding puts each back where it came from.
  if (out.blobVersion >= 201806140 && out.blobVersion < 202012090) {
    if (!readKeywords(out.localKeywords)) return false;
  }

  uint32_t const codeLength = reader.u32();
  if (!reader.ok() || codeLength > reader.remaining()) return false;
  out.code.resize(codeLength);
  if (codeLength > 0 && !reader.raw(out.code.data(), codeLength)) return false;

  // Unity aligns to four bytes after the program bytes, then (in the
  // 2018.06 - 2020.12 format) writes the source map, bind channels and
  // parameter tables. That padding belongs to the code, not to what follows:
  // DXBC is always a whole number of dwords so a PC blob never has any, but
  // GLSL text almost never is, and a translated program whose tables were
  // copied across without it had them read one to three bytes early -- a
  // garbage bind-channel count, and Unity crashed loading the bundle. So the
  // padding is dropped here and WriteSubProgram puts back whatever the new
  // code length needs. An entry that simply ends after unpadded code (nothing
  // else follows) is accepted too.
  {
    size_t const pad = (4 - (codeLength % 4)) % 4;
    reader.skip(std::min(pad, reader.remaining()));
  }

  // Whatever Unity left after that -- any field this code does not model --
  // travels with the program.
  size_t const trailing = reader.remaining();
  out.trailing.resize(trailing);
  if (trailing > 0 && !reader.raw(out.trailing.data(), trailing)) return false;
  return true;
}

// Serializes a sub-program back into the bytes it was read from. Exactly the
// inverse of ReadSubProgram, field for field.
void WriteSubProgram(ShaderSubProgram const& program, std::vector<uint8_t>& out) {
  size_t const start = out.size();
  auto putU32 = [&out](uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  };
  auto align4 = [&out, start]() {
    while ((out.size() - start) % 4 != 0) out.push_back(0);
  };

  putU32(static_cast<uint32_t>(program.blobVersion));
  putU32(static_cast<uint32_t>(program.programType));
  out.insert(out.end(), program.stats.begin(), program.stats.end());

  auto putKeywords = [&](std::vector<std::string> const& keywords) {
    putU32(static_cast<uint32_t>(keywords.size()));
    for (auto const& keyword : keywords) {
      putU32(static_cast<uint32_t>(keyword.size()));
      out.insert(out.end(), keyword.begin(), keyword.end());
      align4();
    }
  };
  putKeywords(program.keywords);
  if (program.blobVersion >= 201806140 && program.blobVersion < 202012090) {
    putKeywords(program.localKeywords);
  }

  putU32(static_cast<uint32_t>(program.code.size()));
  out.insert(out.end(), program.code.begin(), program.code.end());
  align4();
  out.insert(out.end(), program.trailing.begin(), program.trailing.end());
}

}  // namespace

namespace {

void WriteLittleU32(std::vector<uint8_t>& out, size_t offset, uint32_t value) {
  out[offset] = static_cast<uint8_t>(value & 0xffu);
  out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xffu);
  out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xffu);
  out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xffu);
}

}  // namespace

ShaderBodyRewrite BuildShaderObjectBody(uint8_t const* data, size_t size,
                                        ShaderObject const& shader,
                                        std::vector<int32_t> const& platforms,
                                        EncodedProgramStore const& store,
                                        std::vector<BytePatch> const& patches) {
  ShaderBodyRewrite result;
  if (data == nullptr) {
    result.message = "no buffer to rebuild from";
    return result;
  }
  if (shader.bodySize == 0 || shader.bodyFileOffset > size ||
      shader.bodySize > size - shader.bodyFileOffset) {
    result.message = "the shader's body lies outside the file";
    return result;
  }
  if (!store.ok) {
    result.message = "the program store to write back did not encode";
    return result;
  }
  if (!shader.blobPresent) {
    result.message = "the shader carries no compressedBlob to replace";
    return result;
  }
  if (platforms.size() != shader.platforms.size()) {
    result.message = "the converted platform list is a different length than the shader's";
    return result;
  }
  if (shader.platformsTableOffsets.size() != 1) {
    result.message = "the shader's platforms field is not one flat array";
    return result;
  }
  if (store.offsets.size() != shader.offsets.size() ||
      store.compressedLengths.size() != shader.compressedLengths.size() ||
      store.decompressedLengths.size() != shader.decompressedLengths.size()) {
    result.message = "the converted store has a different number of platform groups";
    return result;
  }
  if (shader.offsetsTableOffsets.size() != shader.offsets.size() ||
      shader.compressedLengthsTableOffsets.size() != shader.compressedLengths.size() ||
      shader.decompressedLengthsTableOffsets.size() != shader.decompressedLengths.size()) {
    result.message = "the shader's length tables were not located while parsing";
    return result;
  }
  for (size_t group = 0; group < store.offsets.size(); group++) {
    if (store.offsets[group].size() != shader.offsets[group].size() ||
        store.compressedLengths[group].size() != shader.compressedLengths[group].size() ||
        store.decompressedLengths[group].size() != shader.decompressedLengths[group].size()) {
      result.message = "the converted store has a different number of sub-blobs in group " +
                       std::to_string(group);
      return result;
    }
  }

  size_t const bodyStart = shader.bodyFileOffset;
  size_t const bodyEnd = bodyStart + shader.bodySize;
  if (shader.blobFileOffset < bodyStart + 4 || shader.blobFileOffset > bodyEnd ||
      shader.blobSize > bodyEnd - shader.blobFileOffset) {
    result.message = "the shader's compressedBlob is not inside its own body";
    return result;
  }

  std::vector<uint8_t> body(data + bodyStart, data + bodyEnd);

  auto patchTable = [&](size_t fileOffset, std::vector<int32_t> const& values) -> bool {
    if (fileOffset < bodyStart) return false;
    size_t const relative = fileOffset - bodyStart;
    uint64_t const bytes = static_cast<uint64_t>(values.size()) * 4u;
    if (relative > body.size() || bytes > body.size() - relative) return false;
    for (size_t i = 0; i < values.size(); i++) {
      WriteLittleU32(body, relative + i * 4, static_cast<uint32_t>(values[i]));
    }
    return true;
  };

  // Fixed-width writes into m_ParsedForm. They all sit before the blob, so
  // they are applied to the body before anything in it moves.
  for (auto const& patch : patches) {
    if (patch.bytes.empty()) continue;
    if (patch.fileOffset < bodyStart || patch.fileOffset + patch.bytes.size() > shader.blobFileOffset - 4) {
      result.message = "a program-list patch falls outside m_ParsedForm";
      return result;
    }
    std::memcpy(body.data() + (patch.fileOffset - bodyStart), patch.bytes.data(), patch.bytes.size());
  }

  if (!patchTable(shader.platformsTableOffsets[0], platforms)) {
    result.message = "the platforms array does not fit where it was found";
    return result;
  }
  for (size_t group = 0; group < store.offsets.size(); group++) {
    if (!patchTable(shader.offsetsTableOffsets[group], store.offsets[group]) ||
        !patchTable(shader.compressedLengthsTableOffsets[group], store.compressedLengths[group]) ||
        !patchTable(shader.decompressedLengthsTableOffsets[group],
                    store.decompressedLengths[group])) {
      result.message = "a length table does not fit where it was found";
      return result;
    }
  }

  size_t const relativeBlob = shader.blobFileOffset - bodyStart;
  size_t const oldPadding = shader.blobAligned ? (4 - (shader.blobSize % 4)) % 4 : 0;
  if (relativeBlob + shader.blobSize + oldPadding > body.size()) {
    result.message = "the compressedBlob and its padding run past the end of the object";
    return result;
  }
  size_t const newPadding = shader.blobAligned ? (4 - (store.blob.size() % 4)) % 4 : 0;

  std::vector<uint8_t> rebuilt;
  rebuilt.reserve(body.size() - shader.blobSize + store.blob.size() + 4);
  rebuilt.insert(rebuilt.end(), body.begin(), body.begin() + static_cast<long>(relativeBlob) - 4);
  // The array's element count sits immediately before its data.
  uint8_t countBytes[4];
  uint32_t const count = static_cast<uint32_t>(store.blob.size());
  countBytes[0] = static_cast<uint8_t>(count & 0xffu);
  countBytes[1] = static_cast<uint8_t>((count >> 8) & 0xffu);
  countBytes[2] = static_cast<uint8_t>((count >> 16) & 0xffu);
  countBytes[3] = static_cast<uint8_t>((count >> 24) & 0xffu);
  rebuilt.insert(rebuilt.end(), countBytes, countBytes + 4);
  rebuilt.insert(rebuilt.end(), store.blob.begin(), store.blob.end());
  rebuilt.insert(rebuilt.end(), newPadding, 0u);
  rebuilt.insert(rebuilt.end(), body.begin() + static_cast<long>(relativeBlob + shader.blobSize +
                                                                 oldPadding),
                 body.end());

  result.ok = true;
  result.body = std::move(rebuilt);
  return result;
}

EncodedProgramStore EncodeShaderPrograms(std::vector<int32_t> const& platforms,
                                         std::vector<ShaderSubProgram> const& programs,
                                         std::vector<StoreGroupLayout> const& layouts) {
  EncodedProgramStore store;
  size_t const groupCount = platforms.size();
  store.offsets.resize(groupCount);
  store.compressedLengths.resize(groupCount);
  store.decompressedLengths.resize(groupCount);

  auto putU32 = [](std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
  };

  for (size_t group = 0; group < groupCount; group++) {
    std::vector<ShaderSubProgram const*> members;
    for (auto const& program : programs) {
      if (static_cast<size_t>(program.groupIndex) != group) continue;
      if (program.blobIndex < 0 || program.segment < 0) {
        store.message = "a program carries a negative blob index or segment";
        return store;
      }
      members.push_back(&program);
    }
    StoreGroupLayout layout = group < layouts.size() ? layouts[group] : StoreGroupLayout{};
    if (members.empty()) {
      if (layout.chunkCount > 0) {
        store.message = "platform group " + std::to_string(group) +
                        " had chunks but has no entries left to put in them";
        return store;
      }
      continue;  // a platform with no programs keeps an empty group
    }
    std::stable_sort(members.begin(), members.end(),
                     [](ShaderSubProgram const* a, ShaderSubProgram const* b) {
                       return a->blobIndex < b->blobIndex;
                     });
    for (size_t i = 0; i < members.size(); i++) {
      if (members[i]->blobIndex != static_cast<int32_t>(i)) {
        store.message = "platform group " + std::to_string(group) + " has a hole or a duplicate at entry " +
                        std::to_string(i) + ", so the table would index nothing";
        return store;
      }
    }

    int32_t highestSegment = 0;
    for (auto const* member : members) highestSegment = std::max(highestSegment, member->segment);
    size_t const entrySize = (layout.entrySize == 12 || members.front()->entrySize == 12) ? 12u : 8u;
    size_t const chunkCount =
        std::max<size_t>(static_cast<size_t>(layout.chunkCount), static_cast<size_t>(highestSegment) + 1);
    if (entrySize == 8 && chunkCount > 1) {
      store.message = "8-byte table entries cannot name a segment, but the group has " +
                      std::to_string(chunkCount) + " chunks";
      return store;
    }

    size_t const headerSize = 4 + members.size() * entrySize;
    std::vector<std::vector<uint8_t>> chunks(chunkCount);
    std::vector<uint8_t> table;
    table.reserve(headerSize);
    putU32(table, static_cast<uint32_t>(members.size()));
    for (auto const* member : members) {
      std::vector<uint8_t> body;
      if (member->raw) {
        body = member->rawBytes;
      } else {
        WriteSubProgram(*member, body);
      }
      auto& chunk = chunks[static_cast<size_t>(member->segment)];
      size_t const offset = (member->segment == 0 ? headerSize : 0) + chunk.size();
      putU32(table, static_cast<uint32_t>(offset));
      putU32(table, static_cast<uint32_t>(body.size()));
      if (entrySize == 12) putU32(table, static_cast<uint32_t>(member->segment));
      chunk.insert(chunk.end(), body.begin(), body.end());
    }
    chunks[0].insert(chunks[0].begin(), table.begin(), table.end());

    for (auto& plain : chunks) {
      // A chunk nothing points into still has to exist -- the length tables
      // keep their count -- and LZ4 cannot encode zero bytes.
      if (plain.empty()) plain.push_back(0);
      std::vector<uint8_t> packed(Lz4CompressBound(plain.size()));
      size_t const written = Lz4CompressBlock(plain.data(), plain.size(), packed.data(), packed.size());
      if (written == 0) {
        store.message = "a chunk could not be compressed";
        return store;
      }
      store.offsets[group].push_back(static_cast<int32_t>(store.blob.size()));
      store.compressedLengths[group].push_back(static_cast<int32_t>(written));
      store.decompressedLengths[group].push_back(static_cast<int32_t>(plain.size()));
      store.blob.insert(store.blob.end(), packed.begin(), packed.begin() + static_cast<long>(written));
    }
  }

  store.ok = true;
  return store;
}

namespace {

// Which platform group a program of this type is stored in: the group whose
// platform can run it, or the only group there is.
int32_t GroupForProgramType(std::vector<int32_t> const& platforms, int32_t programType) {
  if (platforms.size() == 1) return 0;
  for (size_t i = 0; i < platforms.size(); i++) {
    int32_t const platform = platforms[i];
    bool const dx = programType >= kGpuProgramDX11VertexSM40 && programType <= kGpuProgramDX11DomainSM50;
    bool const gles = programType >= kGpuProgramGLES31AEP && programType <= kGpuProgramGLES;
    if (dx && platform == kShaderPlatformD3D11) return static_cast<int32_t>(i);
    if (gles && (platform == kShaderPlatformGLES3Plus || platform == kShaderPlatformGLES20)) {
      return static_cast<int32_t>(i);
    }
    if (programType == kGpuProgramSPIRV && platform == kShaderPlatformVulkan) return static_cast<int32_t>(i);
    if ((programType == kGpuProgramMetalVS || programType == kGpuProgramMetalFS) &&
        platform == kShaderPlatformMetal) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

// Reads a group's entry table out of its first chunk, choosing the record width
// the data supports: every record has to land inside the chunk its segment
// names, and records in the first chunk have to lie clear of the table itself.
bool ReadEntryTable(std::vector<std::vector<uint8_t>> const& chunks,
                    std::vector<std::array<uint32_t, 3>>& entries, int32_t& entrySize) {
  if (chunks.empty() || chunks[0].size() < 4) return false;
  auto const& first = chunks[0];
  auto readU32 = [&first](size_t at) {
    return static_cast<uint32_t>(first[at]) | (static_cast<uint32_t>(first[at + 1]) << 8) |
           (static_cast<uint32_t>(first[at + 2]) << 16) | (static_cast<uint32_t>(first[at + 3]) << 24);
  };
  uint32_t const count = readU32(0);
  if (count > first.size() / 8) return false;

  for (size_t width : {size_t(12), size_t(8)}) {
    size_t const header = 4 + static_cast<size_t>(count) * width;
    if (header > first.size()) continue;
    std::vector<std::array<uint32_t, 3>> candidate;
    candidate.reserve(count);
    bool good = true;
    for (uint32_t i = 0; i < count && good; i++) {
      size_t const at = 4 + static_cast<size_t>(i) * width;
      uint32_t const offset = readU32(at);
      uint32_t const length = readU32(at + 4);
      uint32_t const segment = width == 12 ? readU32(at + 8) : 0u;
      if (segment >= chunks.size()) { good = false; break; }
      size_t const chunkSize = chunks[segment].size();
      if (segment == 0 && offset < header) { good = false; break; }
      if (offset > chunkSize || length > chunkSize - offset) { good = false; break; }
      candidate.push_back({offset, length, segment});
    }
    if (good) {
      entries = std::move(candidate);
      entrySize = static_cast<int32_t>(width);
      return true;
    }
  }
  return false;
}

}  // namespace

DecodeResult DecodeShaderPrograms(uint8_t const* data, size_t size, ShaderObject const& shader) {
  DecodeResult result;
  if (data == nullptr) { result.message = "no data"; return result; }
  if (!shader.blobPresent) { result.message = "shader carries no compressedBlob"; return result; }
  if (shader.blobFileOffset > size || shader.blobSize > size - shader.blobFileOffset) {
    result.message = "compressedBlob range lies outside the file";
    return result;
  }
  if (shader.offsets.size() != shader.compressedLengths.size() ||
      shader.offsets.size() != shader.decompressedLengths.size()) {
    result.message = "offsets, compressedLengths and decompressedLengths disagree";
    return result;
  }

  uint8_t const* const blob = data + shader.blobFileOffset;
  size_t const blobSize = shader.blobSize;

  // Entries m_ParsedForm names as parameter blobs, per group. Those are not
  // sub-programs, and parsing one as if it were can "succeed" on garbage.
  std::vector<std::set<uint32_t>> parameterEntries(shader.offsets.size());
  for (auto const& ref : shader.programRefs) {
    if (!ref.hasParameterBlob) continue;
    int32_t const group = GroupForProgramType(shader.platforms, ref.gpuProgramType);
    if (group < 0 || static_cast<size_t>(group) >= parameterEntries.size()) continue;
    parameterEntries[static_cast<size_t>(group)].insert(ref.parameterBlobIndex);
  }

  result.layouts.resize(shader.offsets.size());
  int decoded = 0;
  int failed = 0;
  for (size_t group = 0; group < shader.offsets.size(); group++) {
    // A platform per group is the 2019.3+ layout. A bundle with a single flat
    // group and several platforms cannot say which is which, so it is reported
    // as unknown rather than guessed at.
    int32_t const platform = group < shader.platforms.size() ? shader.platforms[group]
                             : shader.platforms.size() == 1 ? shader.platforms[0]
                                                            : -1;
    auto const& groupOffsets = shader.offsets[group];
    auto const& groupCompressed = shader.compressedLengths[group];
    auto const& groupDecompressed = shader.decompressedLengths[group];
    if (groupOffsets.size() != groupCompressed.size() ||
        groupOffsets.size() != groupDecompressed.size()) {
      failed++;
      continue;
    }
    if (groupOffsets.empty()) continue;

    std::vector<std::vector<uint8_t>> chunks;
    bool chunksOk = true;
    for (size_t entry = 0; entry < groupOffsets.size() && chunksOk; entry++) {
      int32_t const offset = groupOffsets[entry];
      int32_t const compressed = groupCompressed[entry];
      int32_t const decompressedSize = groupDecompressed[entry];
      if (offset < 0 || compressed <= 0 || decompressedSize <= 0 ||
          static_cast<size_t>(offset) > blobSize ||
          static_cast<size_t>(compressed) > blobSize - static_cast<size_t>(offset) ||
          // A decompressed size a bundle claims is an allocation this code is
          // about to make on a headset, so it is capped rather than trusted.
          decompressedSize > 64 * 1024 * 1024) {
        chunksOk = false;
        break;
      }
      std::vector<uint8_t> plain(static_cast<size_t>(decompressedSize));
      size_t const written = Lz4DecodeBlock(blob + offset, static_cast<size_t>(compressed),
                                            plain.data(), plain.size());
      if (written == 0) { chunksOk = false; break; }
      plain.resize(written);
      chunks.push_back(std::move(plain));
    }
    std::vector<std::array<uint32_t, 3>> entries;
    int32_t entryWidth = 8;
    if (!chunksOk || !ReadEntryTable(chunks, entries, entryWidth)) {
      failed++;
      continue;
    }
    result.layouts[group].entrySize = entryWidth;
    result.layouts[group].chunkCount = static_cast<int32_t>(chunks.size());

    for (size_t i = 0; i < entries.size(); i++) {
      auto const& [offset, length, segment] = entries[i];
      ShaderSubProgram program;
      program.platform = platform;
      program.groupIndex = static_cast<int32_t>(group);
      program.blobIndex = static_cast<int32_t>(i);
      program.programIndex = static_cast<int32_t>(i);
      program.segment = static_cast<int32_t>(segment);
      program.entrySize = entryWidth;
      uint8_t const* bytes = chunks[segment].data() + offset;
      bool const isParameters = parameterEntries[group].count(static_cast<uint32_t>(i)) != 0;
      if (isParameters || !ReadSubProgram(bytes, length, program)) {
        // Kept verbatim: a parameter blob, or something this code does not
        // model. Either way its index has to survive, because m_ParsedForm
        // points at it by number.
        program = ShaderSubProgram{};
        program.platform = platform;
        program.groupIndex = static_cast<int32_t>(group);
        program.blobIndex = static_cast<int32_t>(i);
        program.programIndex = static_cast<int32_t>(i);
        program.segment = static_cast<int32_t>(segment);
        program.entrySize = entryWidth;
        program.raw = true;
        program.rawBytes.assign(bytes, bytes + length);
      } else {
        decoded++;
      }
      result.programs.push_back(std::move(program));
    }
  }

  // A group that could not be read cannot be written back, so the result is
  // only usable when every group decoded. A shader with no store at all is not
  // an error.
  result.ok = failed == 0;
  if (failed > 0) {
    result.message = "decoded " + std::to_string(decoded) + " program(s); " +
                     std::to_string(failed) + " platform group(s) could not be read";
  }
  return result;
}

namespace {
// Two parameters name the same thing: by m_NameIndex when both have one, by
// name otherwise. A parameter blob in Unity's inline format carries names and
// no indices, while m_CommonParameters carries indices, resolved to names by
// ResolveNames.
bool SameName(int32_t aIndex, std::string const& aName, int32_t bIndex, std::string const& bName) {
  if (aIndex >= 0 && bIndex >= 0) return aIndex == bIndex;
  return !aName.empty() && aName == bName;
}
}  // namespace

void ProgramParameters::Merge(ProgramParameters const& other) {
  auto addMissing = [](std::vector<ProgramParameter>& into, std::vector<ProgramParameter> const& from) {
    for (auto const& parameter : from) {
      bool present = false;
      for (auto const& existing : into) {
        if (SameName(existing.nameIndex, existing.name, parameter.nameIndex, parameter.name) &&
            existing.index == parameter.index) {
          present = true;
        }
      }
      if (!present) into.push_back(parameter);
    }
  };
  addMissing(vectors, other.vectors);
  addMissing(matrices, other.matrices);
  addMissing(textures, other.textures);
  addMissing(buffers, other.buffers);
  addMissing(uavs, other.uavs);
  addMissing(constantBufferBindings, other.constantBufferBindings);
  for (auto const& buffer : other.constantBuffers) {
    auto it = std::find_if(constantBuffers.begin(), constantBuffers.end(),
                           [&buffer](ProgramConstantBuffer const& b) {
                             return SameName(b.nameIndex, b.name, buffer.nameIndex, buffer.name);
                           });
    if (it == constantBuffers.end()) {
      constantBuffers.push_back(buffer);
    } else {
      addMissing(it->vectors, buffer.vectors);
      addMissing(it->matrices, buffer.matrices);
      it->size = std::max(it->size, buffer.size);
      if (!it->hasStructParams && buffer.hasStructParams) {
        it->structOffset = buffer.structOffset;
        it->structSize = buffer.structSize;
        it->structArraySize = buffer.structArraySize;
      }
      it->hasStructParams = it->hasStructParams || buffer.hasStructParams;
    }
  }
}

void ProgramParameters::ResolveNames(std::map<int32_t, std::string> const& names) {
  auto resolve = [&names](int32_t index, std::string& name) {
    if (!name.empty()) return;
    auto it = names.find(index);
    if (it != names.end()) name = it->second;
  };
  for (auto* list : {&vectors, &matrices, &textures, &buffers, &uavs, &constantBufferBindings}) {
    for (auto& parameter : *list) resolve(parameter.nameIndex, parameter.name);
  }
  for (auto& buffer : constantBuffers) {
    resolve(buffer.nameIndex, buffer.name);
    for (auto& parameter : buffer.vectors) resolve(parameter.nameIndex, parameter.name);
    for (auto& parameter : buffer.matrices) resolve(parameter.nameIndex, parameter.name);
  }
}

namespace {

// Unity's inline parameter layout: the one a 2018.06 - 2020.12 sub-program
// writes after its bind channels, and the one some 2021.3 editors write for a
// whole parameter blob behind a format version (202012090). Names are written
// out as strings rather than as m_NameIndex values.
//
//   int groupCount; groups: string name, int usedSize, int paramCount,
//     params { string name, int type, int rows, int columns, int isMatrix,
//              int arraySize, int index },
//     int structCount, structs { string name, int index, int arraySize,
//              int structSize, int memberCount, members like params },
//     [int isPartialCB -- the 2020.12 format only]
//   int bindingCount; bindings: string name, int type, int index, int extra,
//     [uint textureExtra -- textures only]
//
// Group 0 is the parameters outside any constant buffer; the rest are the
// constant buffers, in order. Binding types: 0 texture, 1 constant buffer
// slot, 2 buffer, 3 UAV, 4 sampler.
//
// withPartialFlag says whether each group ends in the isPartialCB int. Both
// shapes are tried by the caller, which keeps the one that uses up the bytes.
bool ReadInlineParameters(Reader& reader, bool withPartialFlag, ProgramParameters& out) {
  auto readString = [&reader](std::string& into) {
    uint32_t const length = reader.u32();
    if (!reader.ok() || length > reader.remaining()) return false;
    into.assign(length, '\0');
    if (length > 0 && !reader.raw(reinterpret_cast<uint8_t*>(into.data()), length)) return false;
    reader.align4();
    return reader.ok();
  };
  auto readParameter = [&](ProgramParameter& parameter, bool& isMatrix) {
    if (!readString(parameter.name)) return false;
    parameter.type = static_cast<int32_t>(reader.u32());
    int32_t const rows = static_cast<int32_t>(reader.u32());
    int32_t const columns = static_cast<int32_t>(reader.u32());
    isMatrix = static_cast<int32_t>(reader.u32()) > 0;
    parameter.arraySize = static_cast<int32_t>(reader.u32());
    parameter.index = static_cast<int32_t>(reader.u32());
    parameter.dim = isMatrix ? rows : columns;
    return reader.ok();
  };
  // Nothing real comes near these; a count past them means the bytes are not
  // this layout.
  constexpr uint32_t kMaxCount = 4096;

  uint32_t const groupCount = reader.u32();
  if (!reader.ok() || groupCount == 0 || groupCount > kMaxCount) return false;
  for (uint32_t group = 0; group < groupCount; group++) {
    ProgramConstantBuffer buffer;
    if (!readString(buffer.name)) return false;
    buffer.size = static_cast<int32_t>(reader.u32());
    uint32_t const paramCount = reader.u32();
    if (!reader.ok() || paramCount > kMaxCount) return false;
    for (uint32_t i = 0; i < paramCount; i++) {
      ProgramParameter parameter;
      bool isMatrix = false;
      if (!readParameter(parameter, isMatrix)) return false;
      (isMatrix ? buffer.matrices : buffer.vectors).push_back(std::move(parameter));
    }
    uint32_t const structCount = reader.u32();
    if (!reader.ok() || structCount > kMaxCount) return false;
    for (uint32_t i = 0; i < structCount; i++) {
      std::string structName;
      if (!readString(structName)) return false;
      int32_t const structOffset = static_cast<int32_t>(reader.u32());
      int32_t const structArraySize = static_cast<int32_t>(reader.u32());
      int32_t const structSize = static_cast<int32_t>(reader.u32());
      if (!buffer.hasStructParams) {
        buffer.structOffset = structOffset;
        buffer.structArraySize = structArraySize;
        buffer.structSize = structSize;
      }
      uint32_t const memberCount = reader.u32();
      if (!reader.ok() || memberCount > kMaxCount) return false;
      for (uint32_t j = 0; j < memberCount; j++) {
        ProgramParameter member;
        bool isMatrix = false;
        if (!readParameter(member, isMatrix)) return false;
      }
      buffer.hasStructParams = true;
    }
    if (withPartialFlag) reader.u32();
    if (!reader.ok()) return false;
    if (group == 0) {
      for (auto& v : buffer.vectors) out.vectors.push_back(std::move(v));
      for (auto& m : buffer.matrices) out.matrices.push_back(std::move(m));
    } else {
      out.constantBuffers.push_back(std::move(buffer));
    }
  }

  uint32_t const bindingCount = reader.u32();
  if (!reader.ok() || bindingCount > kMaxCount) return false;
  for (uint32_t i = 0; i < bindingCount; i++) {
    ProgramParameter binding;
    if (!readString(binding.name)) return false;
    int32_t const type = static_cast<int32_t>(reader.u32());
    binding.index = static_cast<int32_t>(reader.u32());
    int32_t const extra = static_cast<int32_t>(reader.u32());
    if (!reader.ok()) return false;
    switch (type) {
      case 0: {
        uint32_t const textureExtra = reader.u32();
        binding.samplerIndex = extra;
        binding.dim = static_cast<int32_t>(textureExtra >> 1);
        out.textures.push_back(std::move(binding));
        break;
      }
      case 1: out.constantBufferBindings.push_back(std::move(binding)); break;
      case 2: out.buffers.push_back(std::move(binding)); break;
      case 3: out.uavs.push_back(std::move(binding)); break;
      case 4: break;  // a sampler state; nothing here uses them
      default: return false;
    }
  }
  return reader.ok();
}

// A parameter blob that opens with a sub-program format version is in the
// inline layout rather than the type-tree one; a type-tree blob opens with an
// array length, which is never anywhere near that large.
bool ParseInlineParameterBlob(uint8_t const* data, size_t size, ProgramParameters& out) {
  if (data == nullptr || size < 8) return false;
  uint32_t const version = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  if (version < 201500000u || version > 209912319u) return false;
  for (bool withPartialFlag : {version >= 202012090u, version < 202012090u}) {
    Reader reader(data, size);
    reader.u32();
    ProgramParameters parsed;
    // Only a reading that accounts for every byte (bar alignment) is the
    // right one.
    if (ReadInlineParameters(reader, withPartialFlag, parsed) && reader.remaining() < 4) {
      out = std::move(parsed);
      return true;
    }
  }
  return false;
}

}  // namespace

bool ParseParameterBlob(std::vector<ParameterSchemaNode> const& schema, uint8_t const* data, size_t size,
                        ProgramParameters& out) {
  if (ParseInlineParameterBlob(data, size, out)) return true;
  if (schema.empty() || data == nullptr) return false;
  auto const children = SchemaChildren(schema);
  Reader reader(data, size);
  RecordReader records(reader, schema, children);
  Record root;
  for (size_t child : children[0]) {
    if (!reader.ok()) return false;
    records.Read(child, root);
  }
  if (!reader.ok()) return false;
  for (auto const& [field, list] : root.lists) {
    if (IsParameterField(field)) AddParameterField(field, list, out);
  }
  return true;
}

bool ShaderPlatformRunsOnQuest(int32_t platform) {
  // Quest's Unity player is built against GLES3 and/or Vulkan; nothing else in
  // the list can be handed to an Adreno driver.
  return platform == kShaderPlatformGLES3Plus || platform == kShaderPlatformVulkan;
}

namespace {

// Reads a SerializedFile's header, type table and object table.
//
// This is the one place that knows the file's shape. Both InspectSerializedFile
// (which reads shaders out of it) and RewriteSerializedFile (which rebuilds it
// with new object bodies) go through here, so the two can never drift apart on
// where a field is.
bool ParseLayout(uint8_t const* data, size_t size, Layout& layout) {
  if (data == nullptr || size < 32) return false;

  Reader reader(data, size);
  layout.metadataSize = reader.u32be();
  layout.fileSize = reader.u32be();
  layout.version = reader.u32be();
  layout.dataOffset = reader.u32be();
  if (!reader.ok() || layout.version < 8 || layout.version > 100) return false;

  layout.bigEndian = true;
  if (layout.version >= 9) {
    layout.bigEndian = reader.u8() != 0;
    reader.skip(3);
  }
  if (layout.version >= 22) {
    layout.metadataSize = reader.u32be();
    layout.fileSize = reader.u64be();
    layout.dataOffset = reader.u64be();
    reader.skip(8);  // reserved
  }
  if (!reader.ok()) return false;
  layout.headerLength = reader.position();

  // Same consistency test the converter uses to tell a serialized file apart
  // from a raw .resS payload node that happens to sit in the same archive.
  if (layout.fileSize == 0 || layout.fileSize > size) return false;
  if (layout.dataOffset == 0 || layout.dataOffset > layout.fileSize) return false;
  if (layout.metadataSize == 0 || layout.metadataSize > layout.fileSize) return false;

  reader.setBigEndian(layout.bigEndian);
  layout.isSerializedFile = true;

  if (layout.version >= 7) {
    layout.unityVersion = reader.cstring(64);
    if (!reader.ok() || layout.unityVersion.empty()) {
      layout.message = "unreadable Unity version string";
      return false;
    }
    // This string goes straight into a log line, and a corrupted or hostile
    // bundle can put arbitrary bytes here. Keep it to printable ASCII.
    for (char& c : layout.unityVersion) {
      if (c < 0x20 || c > 0x7e) c = '?';
    }
  }
  if (layout.version >= 8) reader.skip(4);  // m_TargetPlatform

  bool enableTypeTree = true;
  if (layout.version >= 13) enableTypeTree = reader.u8() != 0;
  layout.typeTreePresent = enableTypeTree;

  int32_t const typeCount = static_cast<int32_t>(reader.u32());
  if (!reader.ok() || typeCount < 0 || static_cast<uint32_t>(typeCount) > size) {
    layout.message = "implausible type count";
    return false;
  }

  layout.types.reserve(static_cast<size_t>(typeCount));
  for (int32_t i = 0; i < typeCount && reader.ok(); i++) {
    SerializedTypeInfo type;
    type.classID = static_cast<int32_t>(reader.u32());
    if (layout.version >= 16) reader.u8();                       // isStrippedType
    if (layout.version >= 17) reader.u16();                      // scriptTypeIndex
    if (layout.version >= 13) {
      bool const hasScriptHash = (layout.version < 16 && type.classID < 0) ||
                                 (layout.version >= 16 && type.classID == 114);
      if (hasScriptHash) reader.skip(16);
      reader.skip(16);                                           // old type hash
    }
    if (enableTypeTree) {
      int32_t const nodeCount = static_cast<int32_t>(reader.u32());
      int32_t const stringBufferSize = static_cast<int32_t>(reader.u32());
      if (!reader.ok() || nodeCount < 0 || stringBufferSize < 0 ||
          static_cast<uint32_t>(nodeCount) > size || static_cast<uint32_t>(stringBufferSize) > size) {
        layout.message = "implausible type tree size";
        return false;
      }
      type.nodes.reserve(static_cast<size_t>(nodeCount));
      for (int32_t n = 0; n < nodeCount && reader.ok(); n++) {
        TypeTreeNode node;
        node.version = reader.u16();
        node.level = reader.u8();
        node.typeFlags = reader.u8();
        node.typeStrOffset = reader.u32();
        node.nameStrOffset = reader.u32();
        node.byteSize = static_cast<int32_t>(reader.u32());
        node.index = static_cast<int32_t>(reader.u32());
        node.metaFlag = reader.u32();
        if (layout.version >= 19) reader.skip(8);  // ref type hash
        type.nodes.push_back(node);
      }
      if (!reader.ok()) { layout.message = "truncated type tree"; return false; }
      type.stringBuffer.resize(static_cast<size_t>(stringBufferSize));
      if (stringBufferSize > 0) {
        reader.raw(reinterpret_cast<uint8_t*>(type.stringBuffer.data()),
                   static_cast<size_t>(stringBufferSize));
      }
      if (layout.version >= 21) {
        int32_t const dependencyCount = static_cast<int32_t>(reader.u32());
        if (reader.ok() && dependencyCount > 0 && static_cast<uint32_t>(dependencyCount) < size) {
          reader.skip(static_cast<size_t>(dependencyCount) * 4);
        }
      }
    }
    layout.types.push_back(std::move(type));
  }
  if (!reader.ok()) { layout.message = "truncated type table"; return false; }

  int32_t const objectCount = static_cast<int32_t>(reader.u32());
  if (!reader.ok() || objectCount < 0 || static_cast<uint32_t>(objectCount) > size) {
    layout.message = "implausible object count";
    return false;
  }

  layout.objects.reserve(static_cast<size_t>(objectCount));
  for (int32_t i = 0; i < objectCount && reader.ok(); i++) {
    if (layout.version >= 14) reader.align4();
    ObjectInfo object;
    object.pathID = static_cast<int64_t>(reader.u64());
    object.byteStartFieldOffset = reader.position();
    object.byteStart = layout.version >= 22 ? static_cast<int64_t>(reader.u64())
                                            : static_cast<int64_t>(reader.u32());
    object.byteSizeFieldOffset = reader.position();
    object.byteSize = reader.u32();
    object.typeIndex = static_cast<int32_t>(reader.u32());
    layout.objects.push_back(object);
  }
  if (!reader.ok()) { layout.message = "truncated object table"; return false; }

  layout.parsed = true;
  return true;
}

}  // namespace

FileReport InspectSerializedFile(uint8_t const* data, size_t size) {
  FileReport report;
  Layout layout;
  bool const ok = ParseLayout(data, size, layout);
  report.isSerializedFile = layout.isSerializedFile;
  report.typeTreePresent = layout.typeTreePresent;
  report.unityVersion = layout.unityVersion;
  report.message = layout.message;
  if (!ok) return report;

  report.parsed = true;
  report.objectCount = static_cast<int32_t>(layout.objects.size());
  for (auto const& object : layout.objects) {
    if (object.typeIndex < 0 || static_cast<size_t>(object.typeIndex) >= layout.types.size()) continue;
    SerializedTypeInfo const& type = layout.types[static_cast<size_t>(object.typeIndex)];
    bool const isShader = type.classID == kClassIdShader;
    bool const isTexture = type.classID == kClassIdTexture2D;
    if (!isShader && !isTexture) continue;
    if (isShader) report.shaderObjectCount++;
    if (isTexture) report.textureObjectCount++;
    if (!layout.typeTreePresent || type.nodes.empty()) continue;

    uint64_t const start = static_cast<uint64_t>(object.byteStart) + layout.dataOffset;
    if (start >= size || object.byteSize > size - start) continue;
    if (isShader) {
      ShaderObject shader =
          ReadShaderObject(data + start, object.byteSize, static_cast<size_t>(start), type);
      shader.pathID = object.pathID;
      report.shaders.push_back(std::move(shader));
    } else {
      TextureObject texture =
          ReadTextureObject(data + start, object.byteSize, static_cast<size_t>(start), type);
      texture.pathID = object.pathID;
      report.textures.push_back(std::move(texture));
    }
  }

  if (!layout.typeTreePresent && report.shaderObjectCount > 0) {
    report.message = "type tree stripped: shader objects found but their contents cannot be decoded";
  }
  return report;
}

RewriteResult RewriteSerializedFile(uint8_t const* data, size_t size,
                                    std::vector<ObjectEdit> const& edits) {
  RewriteResult result;
  Layout layout;
  if (!ParseLayout(data, size, layout)) {
    result.message = layout.message.empty() ? "not a serialized file" : layout.message;
    return result;
  }

  // The metadata is copied verbatim, padding included, and only the object
  // table's byteStart/byteSize fields are patched. That is the whole trick:
  // nothing about the type tree, externals, script types or user information
  // changes when an object's *body* changes, so none of it has to be rebuilt --
  // and anything this parser does not understand survives untouched.
  if (layout.dataOffset > size) {
    result.message = "data offset lies outside the file";
    return result;
  }
  std::vector<uint8_t> head(data, data + layout.dataOffset);

  // Objects are laid out in the order they appear in the file, not the order
  // they appear in the table, so that an unedited file rebuilds to the same
  // bytes it started as.
  std::vector<size_t> order(layout.objects.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&layout](size_t a, size_t b) {
    return layout.objects[a].byteStart < layout.objects[b].byteStart;
  });

  // The data region is rebuilt rather than copied, but everything in it that is
  // not an object body is carried over untouched: the gap that preceded each
  // object, and anything past the last one. Two reasons, and the second is the
  // one that matters.
  //
  // With nothing edited, preserving the gaps puts every object back at the
  // offset it already had, so the rewrite reproduces its input byte for byte --
  // which is the only property that can be checked before there is a real
  // shader to write.
  //
  // And a file is not required to be nothing but objects. A header this parser
  // reads but whose object table is empty or unfamiliar would otherwise rebuild
  // to an empty data region, quietly discarding the entire payload. Bytes this
  // code does not understand are kept, not dropped.
  uint64_t const regionSize = layout.fileSize - layout.dataOffset;
  uint8_t const* const region = data + layout.dataOffset;

  std::vector<uint8_t> body;
  int replaced = 0;
  uint64_t previousEnd = 0;
  for (size_t index : order) {
    ObjectInfo& object = layout.objects[index];
    if (object.byteStart < 0 || static_cast<uint64_t>(object.byteStart) < previousEnd) {
      result.message = "objects overlap or run backwards in the data region";
      return result;
    }
    if (static_cast<uint64_t>(object.byteStart) > regionSize ||
        object.byteSize > regionSize - static_cast<uint64_t>(object.byteStart)) {
      result.message = "an object's body lies outside the file";
      return result;
    }

    ObjectEdit const* edit = nullptr;
    for (auto const& candidate : edits) {
      if (candidate.pathID == object.pathID) { edit = &candidate; break; }
    }

    uint64_t const gapStart = previousEnd;
    uint64_t const gap = static_cast<uint64_t>(object.byteStart) - previousEnd;
    body.insert(body.end(), region + gapStart, region + gapStart + gap);
    // Unity puts object bodies on 8-byte boundaries. With the gaps preserved
    // this is already satisfied for an unedited file, so it only bites once a
    // resize has moved something.
    while (body.size() % 8 != 0) body.push_back(0);

    previousEnd = static_cast<uint64_t>(object.byteStart) + object.byteSize;

    uint8_t const* sourceBytes = region + object.byteStart;
    size_t sourceSize = object.byteSize;
    if (edit != nullptr) {
      sourceBytes = edit->body.data();
      sourceSize = edit->body.size();
      replaced++;
    }

    object.byteStart = static_cast<int64_t>(body.size());
    object.byteSize = static_cast<uint32_t>(sourceSize);
    if (sourceSize > 0) body.insert(body.end(), sourceBytes, sourceBytes + sourceSize);
  }

  if (previousEnd < regionSize) {
    body.insert(body.end(), region + previousEnd, region + regionSize);
  }

  if (replaced != static_cast<int>(edits.size())) {
    result.message = "an edit named a pathID this file does not contain";
    return result;
  }

  auto writeField = [&head, &layout](size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; i++) {
      size_t const shift = layout.bigEndian ? 8 * (width - 1 - i) : 8 * i;
      head[offset + i] = static_cast<uint8_t>(value >> shift);
    }
  };

  size_t const startWidth = layout.version >= 22 ? 8 : 4;
  for (auto const& object : layout.objects) {
    if (object.byteStartFieldOffset + startWidth > head.size() ||
        object.byteSizeFieldOffset + 4 > head.size()) {
      result.message = "object table extends past the metadata";
      return result;
    }
    // Before version 22 a byteStart is a 32-bit field. Growing a file past 4GB
    // is not something to discover by silently truncating an offset.
    if (startWidth == 4 && static_cast<uint64_t>(object.byteStart) > 0xffffffffull) {
      result.message = "rewritten file needs 64-bit object offsets, which this file version lacks";
      return result;
    }
    writeField(object.byteStartFieldOffset, static_cast<uint64_t>(object.byteStart), startWidth);
    writeField(object.byteSizeFieldOffset, object.byteSize, 4);
  }

  // The header's size fields are big-endian regardless of the file's own
  // endianness flag, and only the total changes: the metadata kept its length,
  // so the data offset is the same as it was.
  uint64_t const total = layout.dataOffset + body.size();
  auto writeBE = [&head](size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; i++) {
      head[offset + i] = static_cast<uint8_t>(value >> (8 * (width - 1 - i)));
    }
  };
  if (layout.version >= 22) {
    if (head.size() < 48) { result.message = "truncated header"; return result; }
    writeBE(24, total, 8);
  } else {
    if (head.size() < 20) { result.message = "truncated header"; return result; }
    if (total > 0xffffffffull) {
      result.message = "rewritten file exceeds the 32-bit size field of this file version";
      return result;
    }
    writeBE(4, total, 4);
  }

  result.data = std::move(head);
  result.data.insert(result.data.end(), body.begin(), body.end());
  result.ok = true;
  return result;
}

}  // namespace SerializedFileParse
