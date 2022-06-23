#include "gltftangentspace.h"
#include "mikktspace.h"
#include <iostream>
#include <glm/glm.hpp>

struct MikkTSpaceUserData {
    tinygltf::Model& model;
    size_t meshIdx;
    size_t primitiveIdx;

    size_t positionAccessor;
    size_t normalAccessor;
    size_t texcoordAccessor;
    size_t indicesAccessor;

    std::vector<glm::vec3>* tangentBufferData;

    int tangentBufferIdx = -1;
    int tangentBufferViewIdx = -1;
    size_t tangentAccessor;

    tinygltf::Buffer tangentBuffer;
};

bool PrepareUserData(MikkTSpaceUserData* user, size_t meshIdx, size_t primitiveIdx)
{
    user->meshIdx = meshIdx;
    user->primitiveIdx = primitiveIdx;
    auto& model = user->model;
    auto& mesh = user->model.meshes[user->meshIdx];
    auto& primitive = mesh.primitives[user->primitiveIdx];

    if (primitive.mode != TINYGLTF_MODE_TRIANGLES) {
        // Triangle strips and triangle fans could be supported, but not for now.
        std::cout << "Skipping tangent generation for non-triangles mesh (TINYGLTF_MODE_" << primitive.mode << ")\n";
        return 0;
    }

    if (primitive.attributes.contains("TANGENT")) {
        // Primitive already has tangents.
        return 0;
    }

    auto positionAccessor = primitive.attributes.find("POSITION");
    if (positionAccessor == primitive.attributes.end()) {
        return 0;
    }
    user->positionAccessor = positionAccessor->second;

    auto normalAccessor = primitive.attributes.find("NORMAL");
    if (normalAccessor == primitive.attributes.end()) {
        return 0;
    }
    user->normalAccessor = normalAccessor->second;

    auto texcoordAccessor = primitive.attributes.find("TEXCOORD");
    if (texcoordAccessor == primitive.attributes.end()) {
        texcoordAccessor = primitive.attributes.find("TEXCOORD_0");
        if (texcoordAccessor == primitive.attributes.end()) {
            return 0;
        }
    }
    user->texcoordAccessor = texcoordAccessor->second;

    if (primitive.indices == -1) {
        // Not supporting primitives without indices
        return 0;
    }
    user->indicesAccessor = primitive.indices;

    size_t tangentCount = model.accessors[user->positionAccessor].count;
    size_t tangentByteCount = sizeof(glm::vec3) * tangentCount;

    if (user->tangentBufferIdx == -1) {
        tinygltf::Buffer buffer;
        user->model.buffers.push_back(std::move(buffer));
        user->tangentBufferIdx = user->model.buffers.size() - 1;
    }

    auto& buffer = user->model.buffers[user->tangentBufferIdx];
    buffer.data.resize(buffer.data.size() + tangentByteCount);

    tinygltf::BufferView bufferView;
    bufferView.buffer = user->tangentBufferIdx;
    bufferView.byteOffset = user->tangentBuffer.data.size();
    bufferView.byteStride = sizeof(glm::vec3);
    bufferView.dracoDecoded = false;
    bufferView.byteLength = tangentByteCount;

    model.bufferViews.push_back(std::move(bufferView));
    user->tangentBufferViewIdx = model.bufferViews.size();

    tinygltf::Accessor tangentAccessor;
    tangentAccessor.bufferView = user->tangentBufferIdx;
    tangentAccessor.byteOffset = 0;
    tangentAccessor.componentType = TINYGLTF_COMPONENT_TYPE_FLOAT;
    tangentAccessor.type = TINYGLTF_TYPE_VEC3;
    tangentAccessor.normalized = false;
    tangentAccessor.count = tangentCount;

    model.accessors.push_back(std::move(tangentAccessor));
    user->tangentAccessor = model.accessors.size() - 1;
    primitive.attributes["TANGENT"] = user->tangentAccessor;

    return true;
}

// Returns the number of faces (triangles/quads) on the mesh to be processed.
int m_getNumFaces(const SMikkTSpaceContext* pContext)
{
    MikkTSpaceUserData* user = reinterpret_cast<MikkTSpaceUserData*>(pContext->m_pUserData);
    auto& model = user->model;
    auto& mesh = user->model.meshes[user->meshIdx];
    auto& primitive = mesh.primitives[user->primitiveIdx];
    return model.accessors[primitive.indices].count / 3;
}

// Returns the number of vertices on face number iFace
// iFace is a number in the range {0, 1, ..., getNumFaces()-1}
int m_getNumVerticesOfFace(const SMikkTSpaceContext* pContext, const int iFace)
{
    // Only triangles are supported ;)
    return 3;
}

size_t GetAccessorSize(int componentType, int type)
{
    int baseSize = 0;
    switch (componentType)
    {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        baseSize = sizeof(char);
        break;
    case TINYGLTF_COMPONENT_TYPE_SHORT:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        baseSize = sizeof(short);
        break;
    case TINYGLTF_COMPONENT_TYPE_INT:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        baseSize = sizeof(short);
        break;
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        baseSize = sizeof(float);
        break;
    case TINYGLTF_COMPONENT_TYPE_DOUBLE:
        baseSize = sizeof(double);
        break;
    default:
        abort();
    };

    int size;
    switch (type) {
    case TINYGLTF_TYPE_VEC2:
        size = baseSize * 2;
        break;
    case TINYGLTF_TYPE_VEC3:
        size = baseSize * 3;
        break;
    case TINYGLTF_TYPE_VEC4:
        size = baseSize * 4;
        break;
    case TINYGLTF_TYPE_MAT2:
        size = baseSize * 4;
        break;
    case TINYGLTF_TYPE_MAT3:
        size = baseSize * 3 * 3;
        break;
    case TINYGLTF_TYPE_MAT4:
        size = baseSize * 4 * 4;
        break;
    case TINYGLTF_TYPE_SCALAR:
        size = baseSize;
        break;
    default:
        abort();
    }

    return size;
}

void SampleAccessor(const tinygltf::Model& model, const tinygltf::Accessor& accessor, size_t offsetInElements, void* out)
{
    auto& bufferView = model.bufferViews[accessor.bufferView];
    auto& buffer = model.buffers[bufferView.buffer];
    tinygltf::GetNumComponentsInType(accessor.type);
    size_t stride = accessor.ByteStride(bufferView);
    int accessorSize = GetAccessorSize(accessor.componentType, accessor.type);
    size_t byteIndex = bufferView.byteOffset + accessor.byteOffset + (offsetInElements * stride);
    memcpy(out, &buffer.data[byteIndex], accessorSize);
}

void WriteAccessor(tinygltf::Model& model, const tinygltf::Accessor& accessor, size_t offsetInElements, const void* in)
{
    auto& bufferView = model.bufferViews[accessor.bufferView];
    auto& buffer = model.buffers[bufferView.buffer];
    tinygltf::GetNumComponentsInType(accessor.type);
    size_t stride = accessor.ByteStride(bufferView);
    int accessorSize = GetAccessorSize(accessor.componentType, accessor.type);
    size_t byteIndex = bufferView.byteOffset + accessor.byteOffset + (offsetInElements * stride);
    memcpy(reinterpret_cast<void*>(&buffer.data[byteIndex]), in, accessorSize);
}

void GetAttribute(const tinygltf::Model& model, int accessor, int indicesAccessorIdx, float fvAttributeOut[], const int iFace, const int iVert)
{
    auto& attribAccessor = model.accessors[accessor];
    auto& indicesAccessor = model.accessors[indicesAccessorIdx];
    auto& bufferView = model.bufferViews[attribAccessor.bufferView];
    auto& buffer = model.buffers[bufferView.buffer];

    size_t index = iFace * 3 + iVert;
    unsigned int vertex = 0;
    SampleAccessor(model, indicesAccessor, index, &vertex);
    SampleAccessor(model, attribAccessor, vertex, fvAttributeOut);
}

void SetAttribute(tinygltf::Model& model, int accessor, int indicesAccessorIdx, const float fvAttributeIn[], const int iFace, const int iVert)
{
    auto& attribAccessor = model.accessors[accessor];
    auto& indicesAccessor = model.accessors[indicesAccessorIdx];
    auto& bufferView = model.bufferViews[attribAccessor.bufferView];
    auto& buffer = model.buffers[bufferView.buffer];

    size_t index = iFace * 3 + iVert;
    unsigned int vertex = 0;
    SampleAccessor(model, indicesAccessor, index, &vertex);
    WriteAccessor(model, attribAccessor, vertex, fvAttributeIn);
}

// returns the position/normal/texcoord of the referenced face of vertex number iVert.
// iVert is in the range {0,1,2} for triangles and {0,1,2,3} for quads.
void m_getPosition(const SMikkTSpaceContext* pContext, float fvPosOut[], const int iFace, const int iVert)
{
    MikkTSpaceUserData* user = reinterpret_cast<MikkTSpaceUserData*>(pContext->m_pUserData);
    auto& model = user->model;
    GetAttribute(model, user->positionAccessor, user->indicesAccessor, fvPosOut, iFace, iVert);
}

void m_getNormal(const SMikkTSpaceContext* pContext, float fvNormOut[], const int iFace, const int iVert)
{
    MikkTSpaceUserData* user = reinterpret_cast<MikkTSpaceUserData*>(pContext->m_pUserData);
    auto& model = user->model;
    GetAttribute(model, user->normalAccessor, user->indicesAccessor, fvNormOut, iFace, iVert);
}

void m_getTexCoord(const SMikkTSpaceContext* pContext, float fvTexcOut[], const int iFace, const int iVert)
{
    MikkTSpaceUserData* user = reinterpret_cast<MikkTSpaceUserData*>(pContext->m_pUserData);
    auto& model = user->model;
    GetAttribute(model, user->texcoordAccessor, user->indicesAccessor, fvTexcOut, iFace, iVert);
}

// either (or both) of the two setTSpace callbacks can be set.
// The call-back m_setTSpaceBasic() is sufficient for basic normal mapping.

// This function is used to return the tangent and fSign to the application.
// fvTangent is a unit length vector.
// For normal maps it is sufficient to use the following simplified version of the bitangent which is generated at pixel/vertex level.
// bitangent = fSign * cross(vN, tangent);
// Note that the results are returned unindexed. It is possible to generate a new index list
// But averaging/overwriting tangent spaces by using an already existing index list WILL produce INCRORRECT results.
// DO NOT! use an already existing index list.
void m_setTSpaceBasic(const SMikkTSpaceContext* pContext, const float fvTangent[], const float fSign, const int iFace, const int iVert)
{
    MikkTSpaceUserData* user = reinterpret_cast<MikkTSpaceUserData*>(pContext->m_pUserData);
    auto& model = user->model;
    SetAttribute(model, user->tangentBufferIdx, user->indicesAccessor, fvTangent, iFace, iVert);
}

// I implementing this not realizing I would have to de-index and re-index the mesh afterwards.
// Does not work properly as of now.
void AddTangentsToModel(tinygltf::Model& model)
{
    SMikkTSpaceContext context;
    SMikkTSpaceInterface interface {};
    interface.m_getNumFaces = &m_getNumFaces;
    interface.m_getNumVerticesOfFace = &m_getNumVerticesOfFace;
    interface.m_getPosition = &m_getPosition;
    interface.m_getNormal = &m_getNormal;
    interface.m_getTexCoord = &m_getTexCoord;
    interface.m_setTSpaceBasic = &m_setTSpaceBasic;

    MikkTSpaceUserData userData{
        .model = model
    };
    context.m_pInterface = &interface;
    context.m_pUserData = reinterpret_cast<void*>(&userData);

    for (int meshIdx = 0; meshIdx < model.meshes.size(); meshIdx++) {
        auto& mesh = model.meshes[meshIdx];
        for (int primitiveIdx = 0; primitiveIdx < mesh.primitives.size(); primitiveIdx++) {
            if (PrepareUserData(&userData, meshIdx, primitiveIdx)) {
                genTangSpaceDefault(&context);
            }
        }
    }
}