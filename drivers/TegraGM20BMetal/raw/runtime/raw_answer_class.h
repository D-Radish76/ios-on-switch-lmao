// SPDX-License-Identifier: MIT
// The class an asynchronous ask's answer is handled at: its asker's, as an XPC reply's handler runs at its request's
// class. A thread of fixed priority (no class: Core Animation's render thread) is the most urgent there is, as the shader
// compiler ranks it (compiler_pool.h bandOf). The kernel's answer carries no class of its own, so every queue on its way
// to the asker either is the asker's class or runs each block at the class it was submitted for.
#pragma once
#include <Block.h>
#include <dispatch/dispatch.h>
#include <pthread/qos.h>
#include <cstdint>

namespace raw {

constexpr qos_class_t answerClass(uint32_t qos)
{
    return qos == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_USER_INTERACTIVE : qos_class_t(qos);
}

// A serial queue with no class of its own, whose blocks run at the class each was submitted for (asyncAt)
inline dispatch_queue_t answerQueue(const char *label)
{
    return dispatch_queue_create(label, DISPATCH_QUEUE_SERIAL);
}

// `work` on `queue` at the asker's class `qos`. A more urgent block queued behind it raises the queue's drain until that
// block has run (dispatch's override), so a background asker's block never holds back an interactive one.
inline void asyncAt(dispatch_queue_t queue, uint32_t qos, dispatch_block_t work)
{
    dispatch_block_t block =
        dispatch_block_create_with_qos_class(DISPATCH_BLOCK_ENFORCE_QOS_CLASS, answerClass(qos), 0, work);
    dispatch_async(queue, block);
#if !__has_feature(objc_arc)
    Block_release(block);
#endif
}

}  // namespace raw
