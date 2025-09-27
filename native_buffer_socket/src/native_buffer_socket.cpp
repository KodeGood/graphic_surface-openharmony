/*
 * Copyright (c) 2025 Jani Hautakangas <jani@kodegood.com>
 * Licensed under the Apache License, Version 2.0 (the "License");
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "native_buffer_socket.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include <securec.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "buffer_handle.h"
#include "buffer_handle_utils.h"
#include "buffer_log.h"
#include "native_buffer.h"
#include "surface_buffer.h"
#include "surface_type.h"
#include "graphic_common_c.h"

namespace {
constexpr char HEADER_MAGIC[4] = {'O', 'N', 'B', 'H'};
constexpr uint32_t SERIALIZATION_VERSION = 1;
constexpr uint32_t MAX_RESERVE_CAPACITY = 1024;
constexpr size_t MAX_RESERVE_TOTAL = static_cast<size_t>(MAX_RESERVE_CAPACITY) * 2; // fds + ints
constexpr size_t MAX_HANDLE_SIZE = sizeof(BufferHandle) + sizeof(int32_t) * MAX_RESERVE_TOTAL;
constexpr size_t MAX_FDS_TO_TRANSFER = 128; // align with Android SCM_RIGHTS cap

struct alignas(8) SerializedRequestConfig {
    int32_t width;
    int32_t height;
    int32_t strideAlignment;
    int32_t format;
    uint64_t usage;
    int32_t timeout;
    int32_t colorGamut;
    int32_t transform;
    int32_t sourceType;
};
static_assert(std::is_trivially_copyable_v<SerializedRequestConfig>);

struct alignas(8) SerializedBufferInfo {
    char magic[4];
    uint32_t version;
    uint32_t headerSize;
    uint32_t handleSize;
    uint32_t reserveFds;
    uint32_t reserveInts;
    uint32_t seqNum;
    uint32_t scalingMode;
    int32_t surfaceWidth;
    int32_t surfaceHeight;
    int32_t surfaceColorGamut;
    int32_t surfaceTransform;
    int32_t bufferSize;
    int32_t stride;
    int32_t format;
    uint32_t padding;
    uint64_t usage;
    uint64_t phyAddr;
    SerializedRequestConfig requestConfig;
};
static_assert(std::is_trivially_copyable_v<SerializedBufferInfo>);

constexpr size_t MAX_SERIALIZED_SIZE = sizeof(SerializedBufferInfo) + MAX_HANDLE_SIZE;

void CloseFileDescriptorArray(const std::vector<int>& fds)
{
    for (int fd : fds) {
        if (fd >= 0) {
            (void)close(fd);
        }
    }
}

ssize_t SendMsgWithRetry(int sockfd, struct msghdr* msg)
{
    ssize_t sent;
    do {
        sent = sendmsg(sockfd, msg, 0);
    } while (sent < 0 && errno == EINTR);
    return sent;
}

ssize_t RecvMsgWithRetry(int sockfd, struct msghdr* msg)
{
    ssize_t received;
    do {
        received = recvmsg(sockfd, msg, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    return received;
}

void ExtractFdsFromMessage(const struct msghdr& msg, std::vector<int>& outFds)
{
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(const_cast<struct msghdr*>(&msg), cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        size_t payload = static_cast<size_t>(cmsg->cmsg_len >= CMSG_LEN(0) ? cmsg->cmsg_len - CMSG_LEN(0) : 0);
        size_t count = payload / sizeof(int);
        const int* fdData = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        for (size_t i = 0; i < count; ++i) {
            outFds.push_back(fdData[i]);
        }
    }
}
} // namespace

int OH_NativeBuffer_sendHandleToUnixSocket(const OH_NativeBuffer* nb, int sockfd)
{
    if (nb == nullptr || sockfd < 0) {
        BLOGE("sendHandle invalid input nb=%{public}p, sockfd=%{public}d", nb, sockfd);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    const OHOS::SurfaceBuffer* surfaceBuffer = OHOS::SurfaceBuffer::NativeBufferToSurfaceBuffer(nb);
    if (surfaceBuffer == nullptr) {
        BLOGE("sendHandle surface buffer is null");
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    BufferHandle* handle = surfaceBuffer->GetBufferHandle();
    if (handle == nullptr) {
        BLOGE("sendHandle handle is null");
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    if (handle->reserveFds > MAX_RESERVE_CAPACITY || handle->reserveInts > MAX_RESERVE_CAPACITY) {
        BLOGE("sendHandle reserve overflow fds=%{public}u ints=%{public}u", handle->reserveFds, handle->reserveInts);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    size_t handleSize = sizeof(BufferHandle) + sizeof(int32_t) * (handle->reserveFds + handle->reserveInts);
    if (handleSize > MAX_HANDLE_SIZE) {
        BLOGE("sendHandle handle size overflow=%{public}zu", handleSize);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    SerializedBufferInfo header = {};
    (void)memcpy_s(header.magic, sizeof(header.magic), HEADER_MAGIC, sizeof(HEADER_MAGIC));
    header.version = SERIALIZATION_VERSION;
    header.headerSize = static_cast<uint32_t>(sizeof(SerializedBufferInfo));
    header.handleSize = static_cast<uint32_t>(handleSize);
    header.reserveFds = handle->reserveFds;
    header.reserveInts = handle->reserveInts;
    header.seqNum = surfaceBuffer->GetSeqNum();
    header.scalingMode = static_cast<uint32_t>(surfaceBuffer->GetSurfaceBufferScalingMode());
    header.surfaceWidth = surfaceBuffer->GetSurfaceBufferWidth();
    header.surfaceHeight = surfaceBuffer->GetSurfaceBufferHeight();
    header.surfaceColorGamut = static_cast<int32_t>(surfaceBuffer->GetSurfaceBufferColorGamut());
    header.surfaceTransform = static_cast<int32_t>(surfaceBuffer->GetSurfaceBufferTransform());
    header.bufferSize = handle->size;
    header.stride = handle->stride;
    header.format = handle->format;
    header.usage = handle->usage;
    header.phyAddr = handle->phyAddr;

    OHOS::BufferRequestConfig requestConfig = surfaceBuffer->GetBufferRequestConfig();
    header.requestConfig.width = requestConfig.width;
    header.requestConfig.height = requestConfig.height;
    header.requestConfig.strideAlignment = requestConfig.strideAlignment;
    header.requestConfig.format = requestConfig.format;
    header.requestConfig.usage = requestConfig.usage;
    header.requestConfig.timeout = requestConfig.timeout;
    header.requestConfig.colorGamut = static_cast<int32_t>(requestConfig.colorGamut);
    header.requestConfig.transform = static_cast<int32_t>(requestConfig.transform);
    header.requestConfig.sourceType = static_cast<int32_t>(requestConfig.sourceType);

    std::vector<uint8_t> payload(sizeof(header) + handleSize);
    errno_t copyRet = memcpy_s(payload.data(), payload.size(), &header, sizeof(header));
    if (copyRet != EOK) {
        BLOGE("sendHandle header memcpy_s failed, ret=%{public}d", copyRet);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }

    copyRet = memcpy_s(payload.data() + sizeof(header), payload.size() - sizeof(header), handle, handleSize);
    if (copyRet != EOK) {
        BLOGE("sendHandle handle memcpy_s failed, ret=%{public}d", copyRet);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }
    auto* serializedHandle = reinterpret_cast<BufferHandle*>(payload.data() + sizeof(header));
    serializedHandle->virAddr = nullptr;

    std::vector<int> fdsToSend;
    if (handle->fd >= 0) {
        fdsToSend.push_back(handle->fd);
    }
    for (uint32_t i = 0; i < handle->reserveFds; ++i) {
        if (handle->reserve[i] >= 0) {
            fdsToSend.push_back(handle->reserve[i]);
        }
    }
    if (fdsToSend.size() > MAX_FDS_TO_TRANSFER) {
        BLOGE("sendHandle fd count overflow=%{public}zu", fdsToSend.size());
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    struct iovec iov {
        .iov_base = payload.data(),
        .iov_len = payload.size(),
    };

    std::vector<uint8_t> control;
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    if (!fdsToSend.empty()) {
        size_t controlSize = CMSG_SPACE(sizeof(int) * fdsToSend.size());
        control.resize(controlSize, 0);
        msg.msg_control = control.data();
        msg.msg_controllen = controlSize;

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg == nullptr) {
            BLOGE("sendHandle failed to obtain cmsg header");
            return OHOS::SURFACE_ERROR_API_FAILED;
        }
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fdsToSend.size());
        errno_t fdCopyRet = memcpy_s(CMSG_DATA(cmsg), sizeof(int) * fdsToSend.size(), fdsToSend.data(),
            sizeof(int) * fdsToSend.size());
        if (fdCopyRet != EOK) {
            BLOGE("sendHandle memcpy_s fds failed, ret=%{public}d", fdCopyRet);
            return OHOS::SURFACE_ERROR_API_FAILED;
        }
    }

    ssize_t sent = SendMsgWithRetry(sockfd, &msg);
    if (sent < 0 || static_cast<size_t>(sent) != payload.size()) {
        BLOGE("sendHandle sendmsg failed, sent=%{public}zd errno=%{public}d", sent, errno);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }

    return OHOS::SURFACE_ERROR_OK;
}

int OH_NativeBuffer_recvHandleFromUnixSocket(int sockfd, OH_NativeBuffer** out_nb)
{
    if (out_nb == nullptr || sockfd < 0) {
        BLOGE("recvHandle invalid input out_nb=%{public}p sockfd=%{public}d", out_nb, sockfd);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }
    *out_nb = nullptr;

    std::vector<uint8_t> payload(MAX_SERIALIZED_SIZE);
    std::vector<uint8_t> control(CMSG_SPACE(sizeof(int) * MAX_FDS_TO_TRANSFER));

    struct iovec iov {
        .iov_base = payload.data(),
        .iov_len = payload.size(),
    };
    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.data();
    msg.msg_controllen = control.size();

    ssize_t received = RecvMsgWithRetry(sockfd, &msg);
    if (received < 0) {
        BLOGE("recvHandle recvmsg failed, errno=%{public}d", errno);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }
    if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        BLOGE("recvHandle truncated message flags=%{public}d", msg.msg_flags);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }
    if (static_cast<size_t>(received) < sizeof(SerializedBufferInfo)) {
        BLOGE("recvHandle payload too small=%{public}zd", received);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }

    SerializedBufferInfo header = {};
    errno_t headerCopyRet = memcpy_s(&header, sizeof(header), payload.data(), sizeof(header));
    if (headerCopyRet != EOK) {
        BLOGE("recvHandle header memcpy_s failed, ret=%{public}d", headerCopyRet);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }

    if (memcmp(header.magic, HEADER_MAGIC, sizeof(HEADER_MAGIC)) != 0 || header.version != SERIALIZATION_VERSION) {
        BLOGE("recvHandle invalid header magic or version=%{public}u", header.version);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }
    if (header.headerSize != sizeof(SerializedBufferInfo) || header.handleSize > MAX_HANDLE_SIZE) {
        BLOGE("recvHandle header size mismatch headerSize=%{public}u handleSize=%{public}u",
            header.headerSize, header.handleSize);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    size_t expectedSize = static_cast<size_t>(header.headerSize) + static_cast<size_t>(header.handleSize);
    if (expectedSize != static_cast<size_t>(received)) {
        BLOGE("recvHandle size mismatch received=%{public}zd expected=%{public}zu", received, expectedSize);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }
    if (header.reserveFds > MAX_RESERVE_CAPACITY || header.reserveInts > MAX_RESERVE_CAPACITY) {
        BLOGE("recvHandle reserve overflow fds=%{public}u ints=%{public}u", header.reserveFds, header.reserveInts);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    const uint8_t* handleData = payload.data() + header.headerSize;
    auto* serialized = reinterpret_cast<const BufferHandle*>(handleData);

    size_t computedHandleSize = sizeof(BufferHandle) + sizeof(int32_t) *
        (static_cast<size_t>(header.reserveFds) + static_cast<size_t>(header.reserveInts));
    if (header.handleSize != computedHandleSize) {
        BLOGE("recvHandle handle size mismatch header=%{public}u computed=%{public}zu",
            header.handleSize, computedHandleSize);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    if (serialized->reserveFds != header.reserveFds || serialized->reserveInts != header.reserveInts) {
        BLOGE("recvHandle header/handle reserve mismatch hdr(%{public}u,%{public}u) data(%{public}u,%{public}u)",
            header.reserveFds, header.reserveInts, serialized->reserveFds, serialized->reserveInts);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    std::vector<int> receivedFds;
    ExtractFdsFromMessage(msg, receivedFds);

    size_t expectedFds = (serialized->fd >= 0) ? 1 : 0;
    for (uint32_t i = 0; i < serialized->reserveFds; ++i) {
        if (serialized->reserve[i] >= 0) {
            ++expectedFds;
        }
    }
    if (expectedFds > MAX_FDS_TO_TRANSFER) {
        BLOGE("recvHandle expected fd overflow=%{public}zu", expectedFds);
        CloseFileDescriptorArray(receivedFds);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }
    if (receivedFds.size() != expectedFds) {
        BLOGE("recvHandle fd count mismatch expected=%{public}zu actual=%{public}zu", expectedFds, receivedFds.size());
        CloseFileDescriptorArray(receivedFds);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }

    BufferHandle* handle = AllocateBufferHandle(serialized->reserveFds, serialized->reserveInts);
    if (handle == nullptr) {
        BLOGE("recvHandle failed to allocate buffer handle");
        CloseFileDescriptorArray(receivedFds);
        return OHOS::SURFACE_ERROR_NOMEM;
    }

    errno_t copyRet = memcpy_s(handle, sizeof(BufferHandle) + sizeof(int32_t) * (handle->reserveFds + handle->reserveInts),
        serialized, header.handleSize);
    if (copyRet != EOK) {
        BLOGE("recvHandle memcpy_s failed, ret=%{public}d", copyRet);
        CloseFileDescriptorArray(receivedFds);
        FreeBufferHandle(handle);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }
    handle->virAddr = nullptr;

    if (handle->size != header.bufferSize || handle->stride != header.stride ||
        handle->format != header.format || handle->usage != header.usage ||
        handle->phyAddr != header.phyAddr) {
        BLOGE("recvHandle handle/header mismatch");
        CloseFileDescriptorArray(receivedFds);
        FreeBufferHandle(handle);
        return OHOS::SURFACE_ERROR_INVALID_PARAM;
    }

    size_t fdIndex = 0;
    if (handle->fd >= 0 && fdIndex < receivedFds.size()) {
        handle->fd = receivedFds[fdIndex++];
    } else {
        handle->fd = -1;
    }
    for (uint32_t i = 0; i < handle->reserveFds; ++i) {
        if (handle->reserve[i] >= 0 && fdIndex < receivedFds.size()) {
            handle->reserve[i] = receivedFds[fdIndex++];
        } else {
            handle->reserve[i] = -1;
        }
    }

    receivedFds.clear();

    OHOS::sptr<OHOS::SurfaceBuffer> surfaceBuffer = OHOS::SurfaceBuffer::Create();
    if (surfaceBuffer == nullptr) {
        BLOGE("recvHandle failed to create surface buffer");
        FreeBufferHandle(handle);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }

    OHOS::BufferRequestConfig config = {};
    config.width = header.requestConfig.width;
    config.height = header.requestConfig.height;
    config.strideAlignment = header.requestConfig.strideAlignment;
    config.format = header.requestConfig.format;
    config.usage = header.requestConfig.usage;
    config.timeout = header.requestConfig.timeout;
    config.colorGamut = static_cast<OHOS::GraphicColorGamut>(header.requestConfig.colorGamut);
    config.transform = static_cast<OHOS::GraphicTransformType>(header.requestConfig.transform);
    config.sourceType = static_cast<OHOS::GraphicSourceType>(header.requestConfig.sourceType);
    surfaceBuffer->SetBufferRequestConfig(config);

    surfaceBuffer->SetSurfaceBufferWidth(header.surfaceWidth);
    surfaceBuffer->SetSurfaceBufferHeight(header.surfaceHeight);
    surfaceBuffer->SetSurfaceBufferColorGamut(static_cast<OHOS::GraphicColorGamut>(header.surfaceColorGamut));
    surfaceBuffer->SetSurfaceBufferTransform(static_cast<OHOS::GraphicTransformType>(header.surfaceTransform));
    surfaceBuffer->SetSurfaceBufferScalingMode(static_cast<OHOS::ScalingMode>(header.scalingMode));

    surfaceBuffer->SetBufferHandle(handle);

    OH_NativeBuffer* nativeBuffer = surfaceBuffer->SurfaceBufferToNativeBuffer();
    int refRet = OH_NativeBuffer_Reference(nativeBuffer);
    if (refRet != OHOS::SURFACE_ERROR_OK) {
        BLOGE("recvHandle failed to reference native buffer ret=%{public}d", refRet);
        return OHOS::SURFACE_ERROR_API_FAILED;
    }

    *out_nb = nativeBuffer;
    return OHOS::SURFACE_ERROR_OK;
}
