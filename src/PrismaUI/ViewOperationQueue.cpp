#include "ViewOperationQueue.h"
#include "Core.h"

namespace PrismaUI::ViewOperationQueue {
	using namespace Core;

	bool EnqueueOperation(Core::PrismaViewId viewId, OperationFunc operation) {
		if (!operation) {
			logger::error("EnqueueOperation: Attempted to enqueue null operation for view [{}]", viewId);
			return false;
		}

		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			logger::warn("EnqueueOperation: View [{}] not found, operation discarded", viewId);
			return false;
		}

		{
			std::lock_guard lock(viewData->operationMutex);
			
			if (viewData->pendingOperations.size() >= MAX_OPERATIONS_PER_VIEW) {
				logger::error("EnqueueOperation: Queue overflow for view [{}]. Max size: {}. Operation discarded.",
					viewId, MAX_OPERATIONS_PER_VIEW);
				return false;
			}

			viewData->pendingOperations.push(std::move(operation));
			viewData->queuedOperationsCount.fetch_add(1, std::memory_order_relaxed);
			
			logger::debug("EnqueueOperation: Operation enqueued for view [{}]. Queue size: {}",
				viewId, viewData->pendingOperations.size());
		}

		return true;
	}

	void ProcessNextOperation(Core::PrismaViewId viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			return;
		}

		bool expected = false;
		if (!viewData->isProcessingOperation.compare_exchange_strong(expected, true, 
			std::memory_order_acquire, std::memory_order_relaxed)) {
			return;
		}

		OperationFunc operation;
		{
			std::lock_guard lock(viewData->operationMutex);
			if (viewData->pendingOperations.empty()) {
				viewData->isProcessingOperation.store(false, std::memory_order_release);
				return;
			}

			operation = std::move(viewData->pendingOperations.front());
			viewData->pendingOperations.pop();
			viewData->queuedOperationsCount.fetch_sub(1, std::memory_order_relaxed);
			
			logger::debug("ProcessNextOperation: Processing operation for view [{}]. Remaining in queue: {}",
				viewId, viewData->pendingOperations.size());
		}

		ultralightThread.submit([viewId, op = std::move(operation)]() {
			try {
				{
					std::shared_lock lock(viewsMutex);
					auto it = views.find(viewId);
					if (it == views.end()) {
						logger::warn("ProcessNextOperation: View [{}] was destroyed before operation execution", viewId);
						return;
					}
				}

				op();
				
				logger::debug("ProcessNextOperation: Operation completed for view [{}]", viewId);
			}
			catch (const std::exception& e) {
				logger::error("ProcessNextOperation: Exception during operation execution for view [{}]: {}", 
					viewId, e.what());
			}
			catch (...) {
				logger::error("ProcessNextOperation: Unknown exception during operation execution for view [{}]", viewId);
			}

			std::shared_ptr<PrismaView> vd = nullptr;
			{
				std::shared_lock lock(viewsMutex);
				auto it = views.find(viewId);
				if (it != views.end()) {
					vd = it->second;
				}
			}

			if (vd) {
				vd->isProcessingOperation.store(false, std::memory_order_release);
			}
		});
	}

	void ProcessAllViewOperations() {
		std::vector<Core::PrismaViewId> viewIds;
		{
			std::shared_lock lock(viewsMutex);
			viewIds.reserve(views.size());
			for (const auto& pair : views) {
				viewIds.push_back(pair.first);
			}
		}

		for (const auto& viewId : viewIds) {
			ProcessNextOperation(viewId);
		}
	}

	void ClearOperations(Core::PrismaViewId viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			return;
		}

		size_t clearedCount = 0;
		{
			std::lock_guard lock(viewData->operationMutex);
			clearedCount = viewData->pendingOperations.size();
			
			while (!viewData->pendingOperations.empty()) {
				viewData->pendingOperations.pop();
			}
			
			viewData->queuedOperationsCount.store(0, std::memory_order_relaxed);
		}

		if (clearedCount > 0) {
			logger::debug("ClearOperations: Cleared {} pending operation(s) for view [{}]", 
				clearedCount, viewId);
		}
	}

	size_t GetQueueSize(Core::PrismaViewId viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			return 0;
		}

		std::lock_guard lock(viewData->operationMutex);
		return viewData->pendingOperations.size();
	}

	bool IsProcessing(Core::PrismaViewId viewId) {
		std::shared_ptr<PrismaView> viewData = nullptr;
		{
			std::shared_lock lock(viewsMutex);
			auto it = views.find(viewId);
			if (it != views.end()) {
				viewData = it->second;
			}
		}

		if (!viewData) {
			return false;
		}

		return viewData->isProcessingOperation.load(std::memory_order_acquire);
	}
}
