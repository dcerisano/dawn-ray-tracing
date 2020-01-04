// Copyright 2018 The Dawn Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dawn_native/vulkan/RayTracingAccelerationContainerVk.h"
#include "dawn_native/vulkan/ResourceHeapVk.h"
#include "dawn_native/vulkan/StagingBufferVk.h"

#include "dawn_native/vulkan/DeviceVk.h"
#include "dawn_native/vulkan/UtilsVulkan.h"
#include "dawn_native/vulkan/VulkanError.h"

namespace dawn_native { namespace vulkan {

    namespace {

        VkGeometryTypeNV VulkanGeometryType(wgpu::RayTracingAccelerationGeometryType geometryType) {
            switch (geometryType) {
                case wgpu::RayTracingAccelerationGeometryType::Triangles:
                    return VK_GEOMETRY_TYPE_TRIANGLES_NV;
                case wgpu::RayTracingAccelerationGeometryType::Aabbs:
                    return VK_GEOMETRY_TYPE_AABBS_NV;
                default:
                    UNREACHABLE();
            }
        }

        VkIndexType VulkanIndexFormat(wgpu::IndexFormat format) {
            switch (format) {
                case wgpu::IndexFormat::None:
                    return VK_INDEX_TYPE_NONE_NV;
                case wgpu::IndexFormat::Uint16:
                    return VK_INDEX_TYPE_UINT16;
                case wgpu::IndexFormat::Uint32:
                    return VK_INDEX_TYPE_UINT32;
                default:
                    UNREACHABLE();
            }
        }

        VkFormat VulkanVertexFormat(wgpu::VertexFormat format) {
            switch (format) {
                case wgpu::VertexFormat::Float2:
                    return VK_FORMAT_R32G32_SFLOAT;
                case wgpu::VertexFormat::Float3:
                    return VK_FORMAT_R32G32B32_SFLOAT;
                default:
                    UNREACHABLE();
            }
        }

        VkAccelerationStructureTypeNV VulkanAccelerationContainerLevel(
            wgpu::RayTracingAccelerationContainerLevel level) {
            switch (level) {
                case wgpu::RayTracingAccelerationContainerLevel::Bottom:
                    return VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
                case wgpu::RayTracingAccelerationContainerLevel::Top:
                    return VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
                default:
                    UNREACHABLE();
            }
        }

        VkBuildAccelerationStructureFlagBitsNV VulkanBuildAccelerationStructureFlags(
            wgpu::RayTracingAccelerationContainerFlag buildFlags) {
            uint32_t flags = 0;
            if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::AllowUpdate) {
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_NV;
            }
            if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::PreferFastBuild) {
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_NV;
            }
            if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::PreferFastTrace) {
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_NV;
            }
            if (buildFlags & wgpu::RayTracingAccelerationContainerFlag::LowMemory) {
                flags |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_NV;
            }
            return static_cast<VkBuildAccelerationStructureFlagBitsNV>(flags);
        }

        // generates a 4x3 transform matrix in row-major order
        void Fill4x3TransformMatrix(float* out, const Transform3D* translation,
                                     const Transform3D* rotation,
                                     const Transform3D* scale) {
            // make identity
            out[0] = 1.0f;
            out[5] = 1.0f;
            out[10] = 1.0f;
            out[15] = 1.0f;
            // apply translation
            if (translation != nullptr) {
                float x = translation->x;
                float y = translation->y;
                float z = translation->z;
                out[12] = out[0] * x + out[4] * y + out[8] * z + out[12];
                out[13] = out[1] * x + out[5] * y + out[9] * z + out[13];
                out[14] = out[2] * x + out[6] * y + out[10] * z + out[14];
                out[15] = out[3] * x + out[7] * y + out[11] * z + out[15];
            }
            // apply rotation
            if (rotation != nullptr) {
                // TODO: beautify this
                float x = rotation->x;
                float y = rotation->y;
                float z = rotation->z;
                // x rotation
                {
                    float s = sinf(x);
                    float c = cosf(x);
                    float a10 = out[4];
                    float a11 = out[5];
                    float a12 = out[6];
                    float a13 = out[7];
                    float a20 = out[8];
                    float a21 = out[9];
                    float a22 = out[10];
                    float a23 = out[11];
                    out[4] = a10 * c + a20 * s;
                    out[5] = a11 * c + a21 * s;
                    out[6] = a12 * c + a22 * s;
                    out[7] = a13 * c + a23 * s;
                    out[8] = a20 * c - a10 * s;
                    out[9] = a21 * c - a11 * s;
                    out[10] = a22 * c - a12 * s;
                    out[11] = a23 * c - a13 * s;
                }
                // y rotation
                {
                    float s = sinf(y);
                    float c = cosf(y);
                    float a00 = out[0];
                    float a01 = out[1];
                    float a02 = out[2];
                    float a03 = out[3];
                    float a20 = out[8];
                    float a21 = out[9];
                    float a22 = out[10];
                    float a23 = out[11];
                    out[0] = a00 * c - a20 * s;
                    out[1] = a01 * c - a21 * s;
                    out[2] = a02 * c - a22 * s;
                    out[3] = a03 * c - a23 * s;
                    out[8] = a00 * s + a20 * c;
                    out[9] = a01 * s + a21 * c;
                    out[10] = a02 * s + a22 * c;
                    out[11] = a03 * s + a23 * c;
                }
                // z rotation
                {
                    float s = sinf(z);
                    float c = cosf(z);
                    float a00 = out[0];
                    float a01 = out[1];
                    float a02 = out[2];
                    float a03 = out[3];
                    float a10 = out[4];
                    float a11 = out[5];
                    float a12 = out[6];
                    float a13 = out[7];
                    out[0] = a00 * c + a10 * s;
                    out[1] = a01 * c + a11 * s;
                    out[2] = a02 * c + a12 * s;
                    out[3] = a03 * c + a13 * s;
                    out[4] = a10 * c - a00 * s;
                    out[5] = a11 * c - a01 * s;
                    out[6] = a12 * c - a02 * s;
                    out[7] = a13 * c - a03 * s;
                }
            }
            // apply scale
            if (scale != nullptr) {
                float x = scale->x;
                float y = scale->y;
                float z = scale->z;
                out[0] = out[0] * x;
                out[1] = out[1] * x;
                out[2] = out[2] * x;
                out[3] = out[3] * x;
                out[4] = out[4] * y;
                out[5] = out[5] * y;
                out[6] = out[6] * y;
                out[7] = out[7] * y;
                out[8] = out[8] * z;
                out[9] = out[9] * z;
                out[10] = out[10] * z;
                out[11] = out[11] * z;
            }
            // turn into 4x3
            out[3] = out[12];
            out[7] = out[13];
            out[11] = out[14];
            // reset last row
            out[12] = 0.0f;
            out[13] = 0.0f;
            out[14] = 0.0f;
            out[15] = 0.0f;
        }

    }  // anonymous namespace

    // static
    ResultOrError<RayTracingAccelerationContainer*> RayTracingAccelerationContainer::Create(
        Device* device,
        const RayTracingAccelerationContainerDescriptor* descriptor) {
        std::unique_ptr<RayTracingAccelerationContainer> geometry =
            std::make_unique<RayTracingAccelerationContainer>(device, descriptor);
        DAWN_TRY(geometry->Initialize(descriptor));
        return geometry.release();
    }

    MaybeError RayTracingAccelerationContainer::Initialize(
        const RayTracingAccelerationContainerDescriptor* descriptor) {
        Device* device = ToBackend(GetDevice());

        // save container level
        mLevel = VulkanAccelerationContainerLevel(descriptor->level);
        mFlags = VulkanBuildAccelerationStructureFlags(descriptor->flags);

        // validate ray tracing calls
        if (device->fn.CreateAccelerationStructureNV == nullptr) {
            return DAWN_VALIDATION_ERROR("Invalid Call to CreateAccelerationStructureNV");
        }

        // acceleration container holds geometry
        if (descriptor->level == wgpu::RayTracingAccelerationContainerLevel::Bottom) {
            for (unsigned int ii = 0; ii < descriptor->geometryCount; ++ii) {
                RayTracingAccelerationGeometryDescriptor geomDsc = descriptor->geometries[ii];

                VkGeometryNV geometryInfo{};

                // for now, we lock the geometry type to triangle-only
                if (geomDsc.type != wgpu::RayTracingAccelerationGeometryType::Triangles) {
                    return DAWN_VALIDATION_ERROR(
                        "Other Geometry types than 'Triangles' is unsupported");
                }

                Buffer* vertexBuffer = ToBackend(geomDsc.vertexBuffer);
                if (vertexBuffer->GetHandle() == VK_NULL_HANDLE) {
                    return DAWN_VALIDATION_ERROR(
                        "Invalid vertex data");
                }

                geometryInfo.pNext = nullptr;
                geometryInfo.sType = VK_STRUCTURE_TYPE_GEOMETRY_NV;
                geometryInfo.flags = VK_GEOMETRY_OPAQUE_BIT_NV;
                geometryInfo.geometryType = VulkanGeometryType(geomDsc.type);
                // triangle
                geometryInfo.geometry.triangles.sType = VK_STRUCTURE_TYPE_GEOMETRY_TRIANGLES_NV;
                geometryInfo.geometry.triangles.pNext = nullptr;
                geometryInfo.geometry.triangles.vertexData = vertexBuffer->GetHandle();
                geometryInfo.geometry.triangles.vertexOffset = geomDsc.vertexOffset;
                geometryInfo.geometry.triangles.vertexCount = geomDsc.vertexCount;
                geometryInfo.geometry.triangles.vertexStride = geomDsc.vertexStride;
                geometryInfo.geometry.triangles.vertexFormat =
                    VulkanVertexFormat(geomDsc.vertexFormat);
                // index buffer is optional
                if (geomDsc.indexBuffer != nullptr) {
                    Buffer* indexBuffer = ToBackend(geomDsc.indexBuffer);
                    if (indexBuffer->GetHandle() == VK_NULL_HANDLE) {
                        return DAWN_VALIDATION_ERROR("Invalid index data");
                    }
                    geometryInfo.geometry.triangles.indexData = indexBuffer->GetHandle();
                    geometryInfo.geometry.triangles.indexOffset = geomDsc.indexOffset;
                    geometryInfo.geometry.triangles.indexCount = geomDsc.indexCount;
                    geometryInfo.geometry.triangles.indexType =
                        VulkanIndexFormat(geomDsc.indexFormat);
                } else {
                    geometryInfo.geometry.triangles.indexData = VK_NULL_HANDLE;
                    geometryInfo.geometry.triangles.indexOffset = 0;
                    geometryInfo.geometry.triangles.indexCount = 0;
                    geometryInfo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_NV;
                }
                geometryInfo.geometry.triangles.transformData = nullptr;
                geometryInfo.geometry.triangles.transformOffset = 0;
                // aabb
                geometryInfo.geometry.aabbs.sType = VK_STRUCTURE_TYPE_GEOMETRY_AABB_NV;
                geometryInfo.geometry.aabbs.pNext = nullptr;
                geometryInfo.geometry.aabbs.aabbData = VK_NULL_HANDLE;

                mGeometries.push_back(geometryInfo);
            };
        }

        // create the acceleration container
        {
            MaybeError result = CreateAccelerationStructure(descriptor);
            if (result.IsError())
                return result.AcquireError();
        }

        // acceleration container holds instances
        if (descriptor->level == wgpu::RayTracingAccelerationContainerLevel::Top) {
            // create data for instance buffer
            for (unsigned int ii = 0; ii < descriptor->instanceCount; ++ii) {
                RayTracingAccelerationInstanceDescriptor instance = descriptor->instances[ii];
                RayTracingAccelerationContainer* geometryContainer =
                    ToBackend(instance.geometryContainer);
                VkAccelerationInstance instanceData{};
                float transform[16] = {};
                Fill4x3TransformMatrix(transform, instance.transform->translation,
                                       instance.transform->rotation, instance.transform->scale);
                memcpy(&instanceData.transform, transform, sizeof(instanceData.transform));
                instanceData.instanceId = instance.instanceId;
                instanceData.mask = instance.mask;
                instanceData.instanceOffset = instance.instanceOffset;
                instanceData.flags = static_cast<uint32_t>(instance.flags);
                instanceData.accelerationStructureHandle = geometryContainer->GetHandle();
                if (geometryContainer->GetHandle() == 0) {
                    return DAWN_VALIDATION_ERROR("Invalid Acceleration Container Handle");
                }
                mInstances.push_back(instanceData);
            };
        }

        // container requires instance buffer
        if (descriptor->level == wgpu::RayTracingAccelerationContainerLevel::Top) {
            uint64_t bufferSize = descriptor->instanceCount * sizeof(VkAccelerationInstance);

            VkBufferCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            createInfo.pNext = nullptr;
            createInfo.flags = 0;
            createInfo.size = bufferSize;
            createInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            createInfo.queueFamilyIndexCount = 0;
            createInfo.pQueueFamilyIndices = 0;

            DAWN_TRY(CheckVkSuccess(device->fn.CreateBuffer(device->GetVkDevice(), &createInfo,
                                                            nullptr, &mInstanceBuffer),
                                    "vkCreateBuffer"));

            VkMemoryRequirements requirements;
            device->fn.GetBufferMemoryRequirements(device->GetVkDevice(), mInstanceBuffer,
                                                   &requirements);

            DAWN_TRY_ASSIGN(mInstanceResource, device->AllocateMemory(requirements, true));

            DAWN_TRY(CheckVkSuccess(device->fn.BindBufferMemory(
                                        device->GetVkDevice(), mInstanceBuffer,
                                        ToBackend(mInstanceResource.GetResourceHeap())->GetMemory(),
                                        mInstanceResource.GetOffset()),
                                    "vkBindBufferMemory"));

            // copy instance data into instance buffer
            memcpy(mInstanceResource.GetMappedPointer(), mInstances.data(), bufferSize);
        }

        // reserve scratch memory
        {
            MaybeError result = ReserveScratchMemory(descriptor);
            if (result.IsError())
                return result.AcquireError();
        }

        // take handle
        {
            uint64_t handle = 0;
            MaybeError result = FetchHandle(&handle);
            if (result.IsError())
                return result.AcquireError();
            mHandle = handle;
        }

        return {};
    }

    RayTracingAccelerationContainer::~RayTracingAccelerationContainer() {
        Device* device = ToBackend(GetDevice());
        if (mAccelerationStructure != VK_NULL_HANDLE) {
            device->fn.DestroyAccelerationStructureNV(device->GetVkDevice(), mAccelerationStructure,
                                                      nullptr);
            mAccelerationStructure = VK_NULL_HANDLE;
        }
        mScratchMemory = {};
    }

    MaybeError RayTracingAccelerationContainer::ReserveScratchMemory(
        const RayTracingAccelerationContainerDescriptor* descriptor) {
        Device* device = ToBackend(GetDevice());

        // create scratch memory for this container
        uint32_t resultSize =
            GetMemoryRequirementSize(VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV);
        uint32_t buildSize = GetMemoryRequirementSize(
            VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_BUILD_SCRATCH_NV);
        uint32_t updateSize = GetMemoryRequirementSize(
            VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_UPDATE_SCRATCH_NV);

        {
            // allocate scratch result memory
            VkMemoryRequirements2 resultRequirements =
                GetMemoryRequirements(VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_OBJECT_NV);
            resultRequirements.memoryRequirements.size = resultSize;
            DAWN_TRY_ASSIGN(mScratchMemory.result.resource,
                            device->AllocateMemory(resultRequirements.memoryRequirements, false));
            mScratchMemory.result.offset = mScratchMemory.result.resource.GetOffset();

            // allocate scratch build memory
            VkMemoryRequirements2 buildRequirements = GetMemoryRequirements(
                VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_BUILD_SCRATCH_NV);
            buildRequirements.memoryRequirements.size = buildSize;
            DAWN_TRY_ASSIGN(mScratchMemory.build.resource,
                            device->AllocateMemory(buildRequirements.memoryRequirements, false));
            mScratchMemory.build.offset = mScratchMemory.build.resource.GetOffset();
            {
                MaybeError result = CreateBufferFromResourceMemoryAllocation(
                    device, &mScratchMemory.build.buffer, buildSize,
                    VK_BUFFER_USAGE_RAY_TRACING_BIT_NV,
                    mScratchMemory.build.resource);
                if (result.IsError())
                    return result;
            }

            // allocate scratch update memory (if necessary)
            VkMemoryRequirements2 updateRequirements = GetMemoryRequirements(
                VK_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_TYPE_UPDATE_SCRATCH_NV);
            updateRequirements.memoryRequirements.size = updateSize;
            if (updateRequirements.memoryRequirements.size > 0) {
                DAWN_TRY_ASSIGN(
                    mScratchMemory.update.resource,
                    device->AllocateMemory(updateRequirements.memoryRequirements, false));
                mScratchMemory.update.offset = mScratchMemory.update.resource.GetOffset();
            }

        }

        // bind scratch result memory
        VkBindAccelerationStructureMemoryInfoNV memoryBindInfo{};
        memoryBindInfo.sType = VK_STRUCTURE_TYPE_BIND_ACCELERATION_STRUCTURE_MEMORY_INFO_NV;
        memoryBindInfo.accelerationStructure = GetAccelerationStructure();
        memoryBindInfo.memory =
            ToBackend(mScratchMemory.result.resource.GetResourceHeap())->GetMemory();
        memoryBindInfo.memoryOffset = mScratchMemory.result.offset;
        memoryBindInfo.deviceIndexCount = 0;
        memoryBindInfo.pDeviceIndices = nullptr;

        // make sure the memory got allocated properly
        if (memoryBindInfo.memory == VK_NULL_HANDLE) {
            return DAWN_VALIDATION_ERROR("Failed to allocate Scratch Memory");
        }

        DAWN_TRY(CheckVkSuccess(
            device->fn.BindAccelerationStructureMemoryNV(device->GetVkDevice(), 1, &memoryBindInfo),
            "vkBindAccelerationStructureMemoryNV"));

        return {};
    }

    VkMemoryRequirements2 RayTracingAccelerationContainer::GetMemoryRequirements(
        VkAccelerationStructureMemoryRequirementsTypeNV type) const {
        Device* device = ToBackend(GetDevice());

        VkAccelerationStructureMemoryRequirementsInfoNV memoryRequirementsInfo{};
        memoryRequirementsInfo.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV;
        memoryRequirementsInfo.accelerationStructure = mAccelerationStructure;

        VkMemoryRequirements2 memoryRequirements2{};
        memoryRequirements2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
        memoryRequirementsInfo.type = type;

        device->fn.GetAccelerationStructureMemoryRequirementsNV(
            device->GetVkDevice(), &memoryRequirementsInfo, &memoryRequirements2);

        return memoryRequirements2;
    }

    uint32_t RayTracingAccelerationContainer::GetMemoryRequirementSize(
        VkAccelerationStructureMemoryRequirementsTypeNV type) const {
        return GetMemoryRequirements(type).memoryRequirements.size;
    }

    MaybeError RayTracingAccelerationContainer::CreateAccelerationStructure(
        const RayTracingAccelerationContainerDescriptor* descriptor) {
        Device* device = ToBackend(GetDevice());

        VkAccelerationStructureCreateInfoNV accelerationStructureCI{};
        accelerationStructureCI.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV;
        accelerationStructureCI.compactedSize = 0;

        accelerationStructureCI.info = {};
        accelerationStructureCI.info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV;
        accelerationStructureCI.info.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_NV;
        if (descriptor->level == wgpu::RayTracingAccelerationContainerLevel::Top) {
            accelerationStructureCI.info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_NV;
            accelerationStructureCI.info.instanceCount = descriptor->instanceCount;
            accelerationStructureCI.info.geometryCount = 0;
            accelerationStructureCI.info.pGeometries = nullptr;
        } else if (descriptor->level == wgpu::RayTracingAccelerationContainerLevel::Bottom) {
            accelerationStructureCI.info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_NV;
            accelerationStructureCI.info.instanceCount = 0;
            accelerationStructureCI.info.geometryCount = descriptor->geometryCount;
            accelerationStructureCI.info.pGeometries = mGeometries.data();
        } else {
            return DAWN_VALIDATION_ERROR("Invalid Acceleration Container Level");
        }

        MaybeError result = CheckVkSuccess(
            device->fn.CreateAccelerationStructureNV(
                device->GetVkDevice(), &accelerationStructureCI, nullptr, &mAccelerationStructure),
            "vkCreateAccelerationStructureNV");
        if (result.IsError())
            return result;

        return {};
    }

    VkBuffer RayTracingAccelerationContainer::GetInstanceBufferHandle() const {
        return mInstanceBuffer;
    }

    uint32_t RayTracingAccelerationContainer::GetInstanceBufferOffset() const {
        return mInstanceResource.GetOffset();
    }

    MaybeError RayTracingAccelerationContainer::FetchHandle(uint64_t* handle) {
        Device* device = ToBackend(GetDevice());
        MaybeError result = CheckVkSuccess(
            device->fn.GetAccelerationStructureHandleNV(
                device->GetVkDevice(), mAccelerationStructure, sizeof(uint64_t), handle),
            "vkGetAccelerationStructureHandleNV");
        if (result.IsError())
            return result;
        return {};
    }

    uint64_t RayTracingAccelerationContainer::GetHandle() const {
        return mHandle;
    }

    VkAccelerationStructureTypeNV RayTracingAccelerationContainer::GetLevel() const {
        return mLevel;
    }

    VkBuildAccelerationStructureFlagBitsNV RayTracingAccelerationContainer::GetFlags() const {
        return mFlags;
    }

    VkAccelerationStructureNV RayTracingAccelerationContainer::GetAccelerationStructure() const {
        return mAccelerationStructure;
    }

    std::vector<VkGeometryNV>& RayTracingAccelerationContainer::GetGeometries() {
        return mGeometries;
    }

    std::vector<VkAccelerationInstance>& RayTracingAccelerationContainer::GetInstances() {
        return mInstances;
    }

    ScratchMemoryPool RayTracingAccelerationContainer::GetScratchMemory() const {
        return mScratchMemory;
    }

}}  // namespace dawn_native::vulkan
