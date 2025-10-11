#pragma once

#include <functional>
#include <queue>
#include <mutex>
#include <memory>
#include <atomic>

namespace PrismaUI::Core {
	typedef uint64_t PrismaViewId;
	struct PrismaView;
}

namespace PrismaUI::ViewOperationQueue {
	using OperationFunc = std::function<void()>;
	
	constexpr size_t MAX_OPERATIONS_PER_VIEW = 100;

	bool EnqueueOperation(Core::PrismaViewId viewId, OperationFunc operation);
    
	void ProcessNextOperation(Core::PrismaViewId viewId);

	void ProcessAllViewOperations();

	void ClearOperations(Core::PrismaViewId viewId);

	size_t GetQueueSize(Core::PrismaViewId viewId);

	bool IsProcessing(Core::PrismaViewId viewId);
}
