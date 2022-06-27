#pragma once

#include <memory>
#include <vector>

namespace internal {
    template<typename T>
    struct PoolItemDeleter;

    template<class T>
    struct PoolItemDeleterContext {
        T* items;
        bool* liveItems;
        size_t* firstFreeIndex;
    };
};

template<typename T>
using PoolItem = std::unique_ptr<T, internal::PoolItemDeleter<T>>;


template<typename T>
using SharedPoolItem = std::shared_ptr<T>;

template<typename T, size_t N>
class PoolBlock {
public:
    PoolBlock() : liveItems{ false }, deleterContext(new internal::PoolItemDeleterContext<T>) {
        deleterContext->items = items;
        deleterContext->liveItems = liveItems.data();
        deleterContext->firstFreeIndex = &firstFreeIndex;
    }

    ~PoolBlock() {
        for (size_t i = 0; i < N; i++) {
            if (liveItems[i]) {
                items[i].~T();
                liveItems[i] = false;
            }
        }

        // All leftover deleters will see this go null since they have a weak_ptr to it
        deleterContext = nullptr;
    }

    template<typename... Args>
    PoolItem<T> AllocateUnique(Args... constructorArgs)
    {
        T* item = Allocate(constructorArgs...);
        if (!item) {
            return nullptr;
        }

        return PoolItem<T>(item, internal::PoolItemDeleter<T>(std::weak_ptr(deleterContext)));
    }

    template<typename... Args>
    SharedPoolItem<T> AllocateShared(Args... constructorArgs)
    {
        T* item = Allocate(constructorArgs...);
        if (!item) {
            return nullptr;
        }

        return SharedPoolItem<T>(item, internal::PoolItemDeleter<T>(std::weak_ptr(deleterContext)));
    }

    bool HasFreeSpace() const
    {
        return firstFreeIndex != -1;
    }

    T* NextItem(T* item)
    {
        size_t index = item == nullptr ? 0 : (item - &items[0]) + 1;
        for (; index < N; index++) {
            if (liveItems[index]) {
                return &items[index];
            }
        }

        return nullptr;
    }
private:
    template<typename... Args>
    T* Allocate(Args... constructorArgs)
    {
        assert(firstFreeIndex == SIZE_MAX || !liveItems[firstFreeIndex]);
        if (firstFreeIndex == SIZE_MAX) {
            return nullptr;
        } else {
            T* item = &items[firstFreeIndex];
            // Placement new the memory
            new (item) T(constructorArgs...);

            liveItems[firstFreeIndex] = true;

            // Find the next free index.
            size_t i = firstFreeIndex + 1;
            firstFreeIndex = -1;
            for (; i < N; i++) {
                if (!liveItems[i]) {
                    firstFreeIndex = i;
                    break;
                }
            }

            return item;
        }
    }

    // Placed in union to avoid automatic destructor call
    union { T items[N]; };
    std::array<bool, N> liveItems;
    size_t firstFreeIndex = 0;

    std::shared_ptr<internal::PoolItemDeleterContext<T>> deleterContext;
};

namespace internal {
    template<typename T>
    struct PoolItemDeleter
    {
        std::weak_ptr<PoolItemDeleterContext<T>> context;

        // This constructor only exists for unassigned PoolItem references so the
        // compiler doesn't cry. This deleter is not in a valid state with this
        // constructor.
        PoolItemDeleter() {}

        PoolItemDeleter(std::weak_ptr<PoolItemDeleterContext<T>> context) : context(context) {}

        void operator()(T* item) {
            if (auto lock = context.lock()) {
                size_t index = item - lock->items;
                if (!lock->liveItems[index]) {
                    return;
                }
                item->~T();
                lock->liveItems[index] = false;
                if (index <= *lock->firstFreeIndex || *lock->firstFreeIndex == -1) {
                    *lock->firstFreeIndex = index;
                }
            }
        }
    };
}

// A memory pool which allocates into fixed size blocks with reliable memory
// addresses. Items in the pool are returned in unique pointer PoolItem, which
// will reclaim the spot in the pool when the pointer goes out of scope.
template<typename T, size_t BlockSize>
class Pool
{
public:
    struct PoolIter
    {
        size_t blockIndex;
        T* item;

        operator bool() const {
            return item != nullptr;
        }

        T* operator ->() const {
            return item;
        }
    };

    Pool()
    {
        blocks.emplace_back(new PoolBlock<T, BlockSize>);
        activeAllocationBlock = blocks.back().get();
    }

    template<typename... Args>
    PoolItem<T> AllocateUnique(Args... constructorArgs)
    {
        UpdateActiveBlock();
        return activeAllocationBlock->AllocateUnique(constructorArgs...);
    }

    template<typename... Args>
    SharedPoolItem<T> AllocateShared(Args... constructorArgs)
    {
        UpdateActiveBlock();
        return activeAllocationBlock->AllocateShared(constructorArgs...);
    }

    PoolIter Begin()
    {
        PoolIter iter;
        if (blocks.size() == 0) {
            iter.item = nullptr;
            iter.blockIndex = -1;
            return iter;
        }

        iter.blockIndex = 0;
        iter.item = blocks[0]->NextItem(nullptr);

        return iter;
    }

    PoolIter Next(const PoolIter& iter)
    {
        PoolIter next;
        next.item = blocks[iter.blockIndex]->NextItem(iter.item);
        next.blockIndex = iter.blockIndex;
        while (next.item == nullptr && next.blockIndex + 1 < blocks.size()) {
            next.blockIndex++;
            next.item = blocks[next.blockIndex]->NextItem(nullptr);
        }

        return next;
    }
private:

    void UpdateActiveBlock()
    {
        // If we don't have an active block it's a developer error.
        // We can handle it in release builds however.
        assert(activeAllocationBlock);

        if (activeAllocationBlock == nullptr || !activeAllocationBlock->HasFreeSpace()) {
            // Current block is out of space, see if any other blocks have freed space.
            activeAllocationBlock = nullptr;
            for (const auto& block : blocks) {
                if (block->HasFreeSpace()) {
                    activeAllocationBlock = block.get();
                }
            }
        }

        if (activeAllocationBlock == nullptr) {
            // No active blocks have free space, so create another block.
            blocks.emplace_back(new PoolBlock<T, BlockSize>);
            activeAllocationBlock = blocks.back().get();
        }
    }

    std::vector<std::unique_ptr<PoolBlock<T, BlockSize>>> blocks;
    PoolBlock<T, BlockSize>* activeAllocationBlock;
};