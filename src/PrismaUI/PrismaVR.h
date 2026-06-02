#pragma once
// PrismaVR.h — Public interface for VR integration.
// Does NOT include any Prisma headers. Self-contained API.

#include <cstdint>

namespace PrismaVR {

	// Core lifecycle (called from Core.cpp)
	void Initialize();      // Detect VR, load OpenVR interfaces
	void OnFrame();         // Per-frame update from D3DPresent
	void Shutdown();        // Cleanup overlays and state

	// Query
	bool IsVRActive();      // Whether VR runtime is available and running

	// Optional JS API for modders who want custom 3D placement
	void SetViewPosition(uint64_t viewId, float x, float y, float z);
	void SetViewScale(uint64_t viewId, float scale);
	void SetViewWorldLock(uint64_t viewId, bool locked);

} // namespace PrismaVR
