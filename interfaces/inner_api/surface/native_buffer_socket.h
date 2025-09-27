/*
 * Copyright (c) 2025 Jani Hautakangas <jani@kodegood.com>
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
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

#ifndef INTERFACES_INNERKITS_SURFACE_NATIVE_BUFFER_SOCKET_H
#define INTERFACES_INNERKITS_SURFACE_NATIVE_BUFFER_SOCKET_H

#include "native_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sends the buffer handle of a native buffer over a UNIX domain socket.
 *
 * This API serialises the metadata stored in the buffer handle and transfers all
 * associated file descriptors via SCM_RIGHTS ancillary data. The receiver can
 * reconstruct a compatible `OH_NativeBuffer` instance by calling
 * `OH_NativeBuffer_recvHandleFromUnixSocket`.
 *
 * @param nb Pointer to the native buffer whose handle should be sent.
 * @param sockfd Connected UNIX domain socket file descriptor.
 * @return Returns `SURFACE_ERROR_OK` on success; otherwise returns a negative
 *         error code defined in `graphic_common_c.h`.
 */
int OH_NativeBuffer_sendHandleToUnixSocket(const OH_NativeBuffer* nb, int sockfd);

/**
 * @brief Receives a native buffer handle from a UNIX domain socket.
 *
 * The function reconstructs a new `OH_NativeBuffer` instance using the handle
 * data and the received file descriptors. The caller obtains ownership of the
 * returned buffer and must release it with `OH_NativeBuffer_Unreference` when
 * it is no longer needed.
 *
 * @param sockfd Connected UNIX domain socket file descriptor.
 * @param out_nb Address of the pointer that receives the newly created native buffer.
 * @return Returns `SURFACE_ERROR_OK` on success; otherwise returns a negative
 *         error code defined in `graphic_common_c.h` and leaves `out_nb` as `nullptr`.
 */
int OH_NativeBuffer_recvHandleFromUnixSocket(int sockfd, OH_NativeBuffer** out_nb);

#ifdef __cplusplus
}
#endif

#endif // INTERFACES_INNERKITS_SURFACE_NATIVE_BUFFER_SOCKET_H
