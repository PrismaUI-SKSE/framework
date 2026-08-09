#include "ModelPreview.h"

#include <windows.h>

#include <DirectXTK/DDSTextureLoader.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace PrismaUI::ModelPreview {

// ============================================================================
// Section A: ini config
// ============================================================================

static constexpr const char* INI_PATH = ".\\Data\\SKSE\\Plugins\\PrismaUI_ModelPreview.ini";

struct Config {
	bool loaded = false;
	bool enabled = true;
	int rtSize = 512;
	float spinDegPerSec = 45.0f;
	bool dumpRT = false;
};
static Config g_cfg;

static void LoadConfigOnce()
{
	if (g_cfg.loaded) return;
	g_cfg.loaded = true;
	g_cfg.enabled = GetPrivateProfileIntA("General", "bEnabled", 1, INI_PATH) != 0;
	int sz = GetPrivateProfileIntA("General", "iRTSize", 512, INI_PATH);
	g_cfg.rtSize = (sz < 128) ? 128 : (sz > 2048 ? 2048 : sz);
	g_cfg.spinDegPerSec = (float)GetPrivateProfileIntA("General", "iSpinDegPerSec", 45, INI_PATH);
	g_cfg.dumpRT = GetPrivateProfileIntA("General", "bDumpRT", 0, INI_PATH) != 0;
	logger::info("ModelPreview: enabled={} rtSize={} spin={}deg/s dumpRT={}",
		g_cfg.enabled, g_cfg.rtSize, g_cfg.spinDegPerSec, g_cfg.dumpRT);
}

bool Enabled()
{
	LoadConfigOnce();
	return g_cfg.enabled;
}

// ============================================================================
// Section B: minimal OpenVR mirror (ABI-frozen, same technique as PrismaVR.cpp)
// ============================================================================

namespace mpovr {
	typedef uint64_t VROverlayHandle_t;
	enum EVROverlayError { VROverlayError_None = 0 };
	enum ETrackingUniverseOrigin { TrackingUniverseStanding = 1 };
	enum ETextureType { TextureType_DirectX = 0 };
	enum EColorSpace { ColorSpace_Auto = 0 };
	struct HmdMatrix34_t { float m[3][4]; };
	struct Texture_t { void* handle; ETextureType eType; EColorSpace eColorSpace; };

	// IVROverlay_026 vtable slots (matches PrismaVR.cpp OVL_SLOT table)
	namespace SLOT {
		constexpr int CreateOverlay = 1;
		constexpr int DestroyOverlay = 2;
		constexpr int SetOverlaySortOrder = 19;
		constexpr int SetOverlayWidthInMeters = 21;
		constexpr int SetOverlayTransformAbsolute = 32;
		constexpr int ShowOverlay = 43;
		constexpr int HideOverlay = 44;
		constexpr int SetOverlayTexture = 60;
	}

	template <typename Ret, typename... Args>
	static Ret VCall(void* iface, int slot, Args... args)
	{
		void** vtable = *reinterpret_cast<void***>(iface);
		using Fn = Ret (*)(void*, Args...);
		return reinterpret_cast<Fn>(vtable[slot])(iface, args...);
	}

	template <typename... Args>
	static EVROverlayError VCallOvl(void* iface, int slot, const char* funcName, Args... args)
	{
		auto err = VCall<EVROverlayError>(iface, slot, args...);
		if (err != VROverlayError_None)
			logger::warn("ModelPreview: {} failed (error {})", funcName, (int)err);
		return err;
	}
}

// ============================================================================
// Section C: request/result plumbing
// ============================================================================

struct PxRect {
	float x = 0, y = 0, w = 0, h = 0;
};

struct Request {
	uint64_t viewId = 0;
	std::string id;          // optional slot id; "" = the default single preview
	std::string plugin;
	uint32_t localId = 0;
	uint32_t formId = 0;
	PxRect rect;
	float zoom = 1.0f;
	float panX = 0.0f;
	float panY = 0.0f;
	float roll = 0.0f;       // degrees, user-driven screen-plane rotation
	float spin = -10000.0f;  // deg/sec; 0 = static, absent (-10000) = ini default
	float yaw = -10000.0f;   // degrees; -10000 = framework keeps/banks the angle
	float pitch = 0.0f;      // degrees, screen-relative tilt (grab-and-tumble)
	float brightness = 1.0f; // lighting multiplier (1.0 = default)
	bool flip = false;
};

struct PreviewModel;  // built GPU model, defined in Section D

// One live preview: identity, view params, loaded model + pose, GPU target, VR overlay.
// Many can exist at once (keyed by fullKey) so a consumer can show a wall of items.
// Static previews (effective spin 0) render once into their target, then just composite;
// only spinning previews redraw every frame.
struct Preview {
	uint64_t viewId = 0;
	std::string id;
	PxRect rect;
	float zoom = 1.0f, panX = 0.0f, panY = 0.0f, roll = 0.0f, pitch = 0.0f, brightness = 1.0f;
	float spin = -10000.0f, yaw = 0.0f;
	bool flip = false;
	// turntable bookkeeping, per preview, for pause/resume banking
	float lastAngleDeg = 0.0f;
	std::chrono::steady_clock::time_point spinStart;
	bool spinStarted = false;
	// model + pose
	std::string modelKey;          // currently displayed item ("" = none yet)
	std::string wantKey;           // requested-but-not-loaded item
	uint64_t loadGen = 0;          // bumped when the requested item changes (latest-wins per preview)
	std::shared_ptr<PreviewModel> model;
	XMFLOAT3 center = { 0, 0, 0 };
	float radius = 1.0f;
	XMFLOAT4X4 orient = {};
	int kind = 0;
	bool dirty = true;             // needs (re)render into its target
	// per-preview color target (depth buffer is shared; previews render sequentially)
	ComPtr<ID3D11Texture2D> tex;
	ComPtr<ID3D11RenderTargetView> rtv;
	ComPtr<ID3D11ShaderResourceView> srv;
	// VR overlay, one per preview
	mpovr::VROverlayHandle_t vrOverlay = 0;
	bool vrShown = false;
};

// All live previews, keyed by "<viewId>\x1f<id>". Render-thread only (mutated in TickCore).
static std::map<std::string, Preview> g_previews;
static std::atomic<uint64_t> g_genCounter{ 0 };

// Pending edits posted from the JS thread, drained each frame by TickCore.
static std::mutex g_reqMutex;
static std::map<std::string, Request> g_pendingShows;  // keyed by fullKey
static std::vector<std::string> g_pendingHides;         // fullKeys; "<viewId>\x1f*" = whole view

static std::string MakeFullKey(uint64_t viewId, const std::string& id)
{
	return std::to_string(viewId) + "\x1f" + id;
}

// What the item is drives how it gets posed (PreviewKind)
enum : int {
	kKindGeneric = 0,  // as-authored: bowls sit, books lie, potions stand
	kKindElongate = 1, // weapons: longest axis vertical
	kKindShield = 2,   // flat disc stood upright
	kKindArmor = 3,    // body armor: stand up if lying flat (boots/helmets already sit)
	kKindAmmo = 4,     // quivers: stand long axis up + 180 flip (they stand tip-down otherwise)
	kKindClothing = 5, // robes/clothes: GND models lie opposite to cuirasses - stand up with flipped sign
};

struct PreviewModel;  // fully self-contained GPU model built from file bytes

struct LoadResult {
	std::string fullKey;   // which preview this load is for
	uint64_t loadGen = 0;  // the preview's request generation at post time
	std::string key;       // item key (plugin:localId / fid)
	std::shared_ptr<PreviewModel> model;
	int kind = kKindGeneric;
	bool ok = false;
};
static std::mutex g_resultMutex;
static std::deque<LoadResult> g_results;

// Fixed-schema JSON helpers (known keys, no JSON lib in project)
static bool JsonFindValue(const std::string& json, const char* key, size_t& valuePos)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t p = json.find(needle);
	if (p == std::string::npos) return false;
	p = json.find(':', p + needle.size());
	if (p == std::string::npos) return false;
	++p;
	while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\n' || json[p] == '\r')) ++p;
	if (p >= json.size()) return false;
	valuePos = p;
	return true;
}

static std::string JsonGetString(const std::string& json, const char* key)
{
	size_t p;
	if (!JsonFindValue(json, key, p) || json[p] != '"') return "";
	size_t end = json.find('"', p + 1);
	if (end == std::string::npos) return "";
	return json.substr(p + 1, end - p - 1);
}

static double JsonGetNumber(const std::string& json, const char* key, double fallback)
{
	size_t p;
	if (!JsonFindValue(json, key, p)) return fallback;
	return atof(json.c_str() + p);
}

static std::string MakeKey(const Request& r)
{
	char buf[64];
	if (r.formId) {   // exact FormID keys the cache when present — globally unique, matches the resolver
		snprintf(buf, sizeof(buf), "fid:%08X", r.formId);
		return buf;
	}
	if (!r.plugin.empty()) {
		snprintf(buf, sizeof(buf), ":%08X", r.localId);
		return r.plugin + buf;
	}
	snprintf(buf, sizeof(buf), "fid:%08X", r.formId);
	return buf;
}

static std::string LowerStr(const char* s)
{
	std::string r = s ? s : "";
	for (auto& ch : r) ch = (char)tolower((unsigned char)ch);
	return r;
}

// ============================================================================
// Section C2: NIF parsing primitives
// The NIF file is the single source of truth: geometry, layout, transforms,
// bounds and texture paths all come from the file bytes. No engine loader.
// ============================================================================

struct ByteCursor {
	const uint8_t* p;
	size_t n;
	size_t off = 0;

	bool bytes(void* dst, size_t k)
	{
		if (off + k > n) return false;
		memcpy(dst, p + off, k);
		off += k;
		return true;
	}
	template <class T>
	bool get(T& v) { return bytes(&v, sizeof(T)); }
	bool skip(size_t k)
	{
		if (off + k > n) return false;
		off += k;
		return true;
	}
};

// Vertex-desc decoding (nibble 0 = stride/4; nibble k+1 = byte offset/4 of attribute k)
static uint32_t DescStride(uint64_t desc) { return (uint32_t)(desc & 0xF) * 4; }
static uint32_t DescAttrOffset(uint64_t desc, int attr) { return (uint32_t)((desc >> (4 * attr + 2)) & 0x3C); }
static bool DescHasFlag(uint64_t desc, uint32_t flag) { return ((desc >> 44) & flag) != 0; }
// flags: VERTEX=1 UV=2 UV2=4 NORMAL=8 TANGENT=0x10 COLORS=0x20 SKINNED=0x40 FULLPREC=0x400

// Position block size from desc layout (gap to nearest following attribute)
static uint32_t DescPosBytes(uint64_t desc)
{
	uint32_t posBytes = DescStride(desc);
	for (int attr = 1; attr <= 6; ++attr) {
		uint32_t off = DescAttrOffset(desc, attr);
		if (off > 0 && off < posBytes) posBytes = off;
	}
	return posBytes;
}

static bool ReadGameFile(const std::string& path, std::vector<uint8_t>& out)
{
	RE::BSResourceNiBinaryStream s(path.c_str());
	if (!s.good()) return false;
	out.clear();
	uint8_t buf[8192];
	for (;;) {
		uint32_t before = s.tell();
		bool full = s.read(buf, (uint32_t)sizeof(buf));
		uint32_t got = s.tell() - before;
		if (got) out.insert(out.end(), buf, buf + got);
		if (!full) break;
		if (out.size() > (64u << 20)) return false;
	}
	return out.size() > 0x100;
}

// ============================================================================
// Section C3: full NIF model parser
// ============================================================================

struct ParseStats {
	int shapes = 0, fx = 0, skinnedFail = 0, dynamic = 0, dropped = 0;
	bool hasInvMarker = false;  // BSInvMarker found — the game's own inventory pose
	float invRot[3] = {};       // radians (file stores 1/1000 rad)
};

struct CpuShape {
	std::string name;
	RE::NiTransform world;
	uint64_t desc = 0;
	uint32_t numTris = 0, numVerts = 0, stride = 0;
	std::vector<uint8_t> vertexData, indexData;
	float boundC[3] = {};
	float boundR = 0;
	float mn[3] = {}, mx[3] = {};  // local AABB decoded from the vertex block
	bool alphaTest = false, alphaBlend = false, effect = false;
	std::string texPath;
};

static bool DecodeLocalAabb(const std::vector<uint8_t>& v, uint64_t desc, uint32_t stride, uint32_t count, float mnOut[3], float mxOut[3])
{
	if (!stride || !count || v.size() < (size_t)stride * count) return false;
	bool floatPos = DescPosBytes(desc) >= 16;
	float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (uint32_t i = 0; i < count; ++i) {
		const uint8_t* p = v.data() + (size_t)i * stride;
		float c[3];
		if (floatPos) {
			memcpy(c, p, 12);
		} else {
			const uint16_t* h = reinterpret_cast<const uint16_t*>(p);
			for (int k = 0; k < 3; ++k) c[k] = DirectX::PackedVector::XMConvertHalfToFloat(h[k]);
		}
		for (int k = 0; k < 3; ++k) {
			if (!std::isfinite(c[k]) || fabsf(c[k]) > 1e5f) return false;
			mn[k] = (c[k] < mn[k]) ? c[k] : mn[k];
			mx[k] = (c[k] > mx[k]) ? c[k] : mx[k];
		}
	}
	memcpy(mnOut, mn, 12);
	memcpy(mxOut, mx, 12);
	return true;
}

// NiAVObject base fields shared by nodes and shapes
struct AvBase {
	uint32_t nameIdx = 0xFFFFFFFF;
	uint32_t flags = 0;
	RE::NiTransform xf;
};

static bool ReadAvBase(ByteCursor& b, AvBase& out, bool le)
{
	uint32_t nExtra = 0;
	int32_t controller = -1, collision = -1;
	if (!b.get(out.nameIdx) || !b.get(nExtra) || nExtra > 1000 || !b.skip(nExtra * 4ull)) return false;
	if (!b.get(controller) || !b.get(out.flags)) return false;
	float t[3], r[9], s;
	if (!b.bytes(t, 12) || !b.bytes(r, 36) || !b.get(s)) return false;
	out.xf.translate = { t[0], t[1], t[2] };
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j) out.xf.rotate.entry[i][j] = r[i * 3 + j];
	out.xf.scale = s;
	// Only pre-Skyrim streams (bsver <= 34, FO3 era) keep a property ref list on the
	// AVObject. Skyrim LE (bsver 83) does NOT - its next field is the collision ref,
	// typically -1, which a wrongly-assumed property count reads as 4 billion â†’ bail.
	if (le) {
		uint32_t nProps = 0;
		if (!b.get(nProps) || nProps > 1000 || !b.skip(nProps * 4ull)) return false;
	}
	return b.get(collision);
}

// LE geometry data: plain float arrays, repacked into the SSE-style interleaved
// layout the renderer already consumes (pos half4 @0, uv half2 @8, normal bytes @12)
static constexpr uint64_t LE_DESC = 4ull | (2ull << 8) | (3ull << 16) | (0xBull << 44);

static bool ParseLeTriShapeData(ByteCursor b, CpuShape& cs, bool strips = false, bool matCrc = true)
{
	using DirectX::PackedVector::XMConvertFloatToHalf;
	int32_t groupId = 0;
	uint16_t nv = 0;
	uint8_t keep = 0, compress = 0, hasVerts = 0;
	if (!b.get(groupId) || !b.get(nv) || !b.get(keep) || !b.get(compress) || !b.get(hasVerts)) return false;
	if (!nv || !hasVerts) return false;

	std::vector<float> pos((size_t)nv * 3);
	if (!b.bytes(pos.data(), pos.size() * 4)) return false;

	uint16_t vecFlags = 0;
	if (!b.get(vecFlags)) return false;
	uint32_t numUv = vecFlags & 0x3F;
	// Skyrim-era streams (bsver > 34) carry a Material CRC here; reading it as
	// hasNormals (first byte is usually 0) silently corrupted the rest of the walk.
	if (matCrc) {
		uint32_t crc = 0;
		if (!b.get(crc)) return false;
	}
	uint8_t hasNormals = 0;
	if (!b.get(hasNormals)) return false;
	std::vector<float> nrm;
	if (hasNormals) {
		nrm.resize((size_t)nv * 3);
		if (!b.bytes(nrm.data(), nrm.size() * 4)) return false;
		if (vecFlags & 0x1000 && !b.skip((size_t)nv * 24)) return false;  // tangents + bitangents
	}
	if (!b.bytes(cs.boundC, 12) || !b.get(cs.boundR)) return false;
	uint8_t hasColors = 0;
	if (!b.get(hasColors)) return false;
	if (hasColors && !b.skip((size_t)nv * 16)) return false;
	std::vector<float> uv;
	if (numUv) {
		uv.resize((size_t)nv * 2);
		if (!b.bytes(uv.data(), uv.size() * 4)) return false;
		for (uint32_t extra = 1; extra < numUv; ++extra)
			if (!b.skip((size_t)nv * 8)) return false;
	}
	uint16_t consistency = 0;
	int32_t additional = -1;
	if (!b.get(consistency) || !b.get(additional)) return false;
	uint16_t numTris = 0;
	if (strips) {
		// NiTriStripsData: unroll triangle strips into a plain triangle list
		uint16_t declTris = 0, numStrips = 0;
		if (!b.get(declTris) || !b.get(numStrips) || !numStrips || numStrips > 4096) return false;
		std::vector<uint16_t> lens(numStrips);
		if (!b.bytes(lens.data(), (size_t)numStrips * 2)) return false;
		uint8_t hasPoints = 0;
		if (!b.get(hasPoints) || !hasPoints) return false;
		std::vector<uint16_t> tris;
		tris.reserve((size_t)declTris * 3);
		std::vector<uint16_t> strip;
		for (uint16_t s = 0; s < numStrips; ++s) {
			strip.resize(lens[s]);
			if (lens[s] && !b.bytes(strip.data(), (size_t)lens[s] * 2)) return false;
			for (uint32_t k = 2; k < lens[s]; ++k) {
				uint16_t t0 = strip[k - 2], t1 = strip[k - 1], t2 = strip[k];
				if (t0 == t1 || t1 == t2 || t0 == t2) continue;  // degenerate (strip restart)
				if (k & 1) { tris.push_back(t0); tris.push_back(t2); tris.push_back(t1); }
				else       { tris.push_back(t0); tris.push_back(t1); tris.push_back(t2); }
			}
		}
		if (tris.empty() || tris.size() / 3 > 0xFFFF) return false;
		numTris = (uint16_t)(tris.size() / 3);
		cs.indexData.resize(tris.size() * 2);
		memcpy(cs.indexData.data(), tris.data(), cs.indexData.size());
	} else {
		uint32_t numTriPoints = 0;
		uint8_t hasTris = 0;
		if (!b.get(numTris) || !b.get(numTriPoints) || !b.get(hasTris)) return false;
		if (!numTris || !hasTris) return false;
		cs.indexData.resize((size_t)numTris * 6);
		if (!b.bytes(cs.indexData.data(), cs.indexData.size())) return false;
	}

	// Interleave into the synthetic SSE layout
	cs.desc = LE_DESC;
	cs.stride = 16;
	cs.numVerts = nv;
	cs.numTris = numTris;
	cs.vertexData.resize((size_t)nv * 16);
	float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	for (uint32_t i = 0; i < nv; ++i) {
		uint8_t* v = cs.vertexData.data() + (size_t)i * 16;
		uint16_t* hp = reinterpret_cast<uint16_t*>(v);
		for (int k = 0; k < 3; ++k) {
			float c = pos[(size_t)i * 3 + k];
			if (!std::isfinite(c) || fabsf(c) > 1e5f) return false;
			hp[k] = XMConvertFloatToHalf(c);
			mn[k] = (c < mn[k]) ? c : mn[k];
			mx[k] = (c > mx[k]) ? c : mx[k];
		}
		hp[3] = 0;
		uint16_t* up = reinterpret_cast<uint16_t*>(v + 8);
		up[0] = XMConvertFloatToHalf(uv.empty() ? 0.5f : uv[(size_t)i * 2]);
		up[1] = XMConvertFloatToHalf(uv.empty() ? 0.5f : uv[(size_t)i * 2 + 1]);
		uint8_t* np = v + 12;
		if (!nrm.empty()) {
			for (int k = 0; k < 3; ++k) {
				float n = nrm[(size_t)i * 3 + k] * 0.5f + 0.5f;
				np[k] = (uint8_t)((n < 0 ? 0 : (n > 1 ? 1 : n)) * 255.0f);
			}
		} else {
			np[0] = np[1] = 128;
			np[2] = 255;
		}
		np[3] = 0;
	}
	memcpy(cs.mn, mn, 12);
	memcpy(cs.mx, mx, 12);
	return true;
}

// NiObjectNET base (properties have no transform)
static bool ReadObjectNet(ByteCursor& b)
{
	uint32_t nameIdx = 0, nExtra = 0;
	int32_t controller = -1;
	return b.get(nameIdx) && b.get(nExtra) && nExtra <= 1000 && b.skip(nExtra * 4ull) && b.get(controller);
}

// Skin partition entry: walk the variable-length arrays to the triangle list.
// Indices may be partition-local (vertex map present) and need remapping.
struct SkinTriRef {
	size_t triOff = 0;
	uint32_t triCount = 0;
	size_t mapOff = 0;
	uint32_t mapLen = 0;
};

static bool SkinTriangles(ByteCursor pc, uint32_t bufferVerts, int wBytes, SkinTriRef& out)
{
	uint16_t nv2 = 0, nt2 = 0, nb = 0, ns = 0, nw = 0;
	if (!pc.get(nv2) || !pc.get(nt2) || !pc.get(nb) || !pc.get(ns) || !pc.get(nw)) return false;
	if (!nv2 || nv2 > bufferVerts || !nt2 || ns != 0) return false;
	if (!pc.skip((size_t)nb * 2)) return false;
	uint8_t hasMap = 0;
	if (!pc.get(hasMap)) return false;
	if (hasMap) {
		out.mapOff = pc.off;
		out.mapLen = nv2;
		if (!pc.skip((size_t)nv2 * 2)) return false;
	}
	uint8_t hasW = 0;
	if (!pc.get(hasW)) return false;
	if (hasW && !pc.skip((size_t)nv2 * nw * wBytes)) return false;
	uint8_t hasFaces = 0;
	if (!pc.get(hasFaces) || !hasFaces) return false;
	out.triOff = pc.off;
	out.triCount = nt2;
	return pc.skip((size_t)nt2 * 6);
}

// Extract + remap + validate the index list; a wrong layout guess fails here
static bool ExtractSkinIndices(const std::vector<uint8_t>& data, ByteCursor pc, uint32_t bufferVerts, int wBytes,
	std::vector<uint8_t>& idxOut, uint32_t& triCountOut)
{
	SkinTriRef tr;
	if (!SkinTriangles(pc, bufferVerts, wBytes, tr)) return false;
	idxOut.resize((size_t)tr.triCount * 6);
	ByteCursor ti{ data.data(), data.size(), tr.triOff };
	if (!ti.bytes(idxOut.data(), idxOut.size())) return false;

	uint16_t* ind = reinterpret_cast<uint16_t*>(idxOut.data());
	size_t n = (size_t)tr.triCount * 3;
	if (tr.mapLen) {
		std::vector<uint16_t> map(tr.mapLen);
		ByteCursor mc{ data.data(), data.size(), tr.mapOff };
		if (!mc.bytes(map.data(), (size_t)tr.mapLen * 2)) return false;
		for (size_t i = 0; i < n; ++i) {
			if (ind[i] >= tr.mapLen) return false;
			ind[i] = map[ind[i]];
		}
	}
	for (size_t i = 0; i < n; ++i)
		if (ind[i] >= bufferVerts) return false;
	triCountOut = tr.triCount;
	return true;
}

static bool ParseNifModel(const std::vector<uint8_t>& data, std::vector<CpuShape>& out, ParseStats& st)
{
	ByteCursor c{ data.data(), data.size() };
	{
		uint8_t ch = 0;
		size_t guard = 0;
		do {
			if (!c.get(ch)) return false;
		} while (ch != '\n' && ++guard < 256);
	}
	uint32_t ver, userver, nblocks, bsver;
	uint8_t endian;
	if (!c.get(ver) || !c.get(endian) || !c.get(userver) || !c.get(nblocks) || !c.get(bsver)) return false;
	if (nblocks == 0 || nblocks > 100000) return false;
	const bool leProps = bsver <= 34;  // only FO3-era streams carry AVObject property lists
	for (int i = 0; i < 3; ++i) {
		uint8_t len;
		if (!c.get(len) || !c.skip(len)) return false;
	}
	uint16_t ntypes;
	if (!c.get(ntypes)) return false;
	std::vector<std::string> types(ntypes);
	for (auto& t : types) {
		uint32_t len;
		if (!c.get(len) || len > 1000) return false;
		t.resize(len);
		if (len && !c.bytes(t.data(), len)) return false;
	}
	std::vector<uint16_t> tidx(nblocks);
	if (!c.bytes(tidx.data(), nblocks * 2ull)) return false;
	std::vector<uint32_t> sizes(nblocks);
	if (!c.bytes(sizes.data(), nblocks * 4ull)) return false;
	uint32_t nstr, maxlen;
	if (!c.get(nstr) || !c.get(maxlen) || nstr > 100000) return false;
	std::vector<std::string> strings(nstr);
	for (auto& s : strings) {
		uint32_t len;
		if (!c.get(len) || len > 4096) return false;
		s.resize(len);
		if (len && !c.bytes(s.data(), len)) return false;
	}
	uint32_t ngroups;
	if (!c.get(ngroups) || !c.skip(ngroups * 4ull)) return false;

	std::vector<size_t> blockOff(nblocks);
	{
		size_t o = c.off;
		for (uint32_t i = 0; i < nblocks; ++i) {
			blockOff[i] = o;
			o += sizes[i];
		}
	}

	auto typeOf = [&](int32_t i) -> const std::string& {
		static const std::string none;
		if (i < 0 || (uint32_t)i >= nblocks || tidx[i] >= types.size()) return none;
		return types[tidx[i]];
	};
	auto nameOf = [&](uint32_t idx) -> std::string {
		return (idx < strings.size()) ? strings[idx] : std::string();
	};
	auto cursorAt = [&](int32_t i) { return ByteCursor{ data.data(), data.size(), blockOff[i] }; };

	// --- Pass 1: classify and parse blocks ---
	struct NodeRec {
		AvBase av;
		std::vector<int32_t> children;
	};
	struct PendingShape {
		AvBase av;
		int32_t skinRef = -1, shaderRef = -1, alphaRef = -1;
		CpuShape cs;
		bool valid = false;
	};
	std::map<int32_t, NodeRec> nodes;
	std::map<int32_t, PendingShape> shapes;
	std::map<int32_t, std::pair<bool, bool>> alphas;       // blockIdx -> {blend, test}
	std::map<int32_t, std::vector<std::string>> texSets;   // blockIdx -> texture paths

	for (uint32_t i = 0; i < nblocks; ++i) {
		const std::string& t = typeOf((int32_t)i);
		bool isNode = !t.empty() &&
		              (t.find("Node") != std::string::npos || t == "BSDamageStage" || t == "BSFadeNode");
		if (isNode) {
			ByteCursor b = cursorAt((int32_t)i);
			NodeRec nr;
			uint32_t nChildren = 0;
			if (ReadAvBase(b, nr.av, leProps) && b.get(nChildren) && nChildren < 10000) {
				nr.children.resize(nChildren);
				if (nChildren == 0 || b.bytes(nr.children.data(), nChildren * 4ull))
					nodes.emplace((int32_t)i, std::move(nr));
			}
		} else if (t == "NiTriShape" || t == "NiTriStrips") {
			// LE-format geometry (Cathedral Armory, unconverted vanilla leftovers like
			// Steel/Elven shields and amulets) - repacked at parse time. Strips unroll
			// to triangle lists inside ParseLeTriShapeData.
			const bool isStrips = (t == "NiTriStrips");
			ByteCursor b = cursorAt((int32_t)i);
			PendingShape ps;
			if (!ReadAvBase(b, ps.av, leProps)) { st.dropped++; continue; }
			int32_t dataRef = -1, skinRef = -1;
			uint32_t nMats = 0;
			if (!b.get(dataRef) || !b.get(skinRef)) continue;
			if (!b.get(nMats) || nMats > 64 || !b.skip((size_t)nMats * 8ull)) continue;
			if (!b.skip(4 + 1)) continue;  // active material, needs-update
			if (!b.get(ps.shaderRef) || !b.get(ps.alphaRef)) continue;
			if (skinRef >= 0) {
				// Low-bone skins are rigid physics clutter (Divine amulets, lanterns):
				// each piece dangles on a hinge bone but the geometry doesn't deform, so
				// the bind-pose NiTriShapeData renders correctly as a static preview (the
				// dangle isn't needed). A full chain+pendant amulet is ~3 bones. Genuinely
				// deformable skins (full-body armor) use 20-60 bones and stay rejected to
				// avoid a distorted render.
				uint32_t boneCount = 0;
				const std::string& skinType = typeOf(skinRef);
				if (skinType == "NiSkinInstance" || skinType == "BSDismemberSkinInstance") {
					ByteCursor sb = cursorAt(skinRef);
					if (sb.skip(12)) sb.get(boneCount); // past data/partition/skeleton refs
				}
				if (boneCount == 0 || boneCount > 16) {
					st.skinnedFail++;
					continue;
				}
				// rigid physics clutter: fall through and parse the geometry as static
			}
			if (typeOf(dataRef) != (isStrips ? "NiTriStripsData" : "NiTriShapeData")) {
				st.dropped++;
				continue;
			}
			ps.cs.name = nameOf(ps.av.nameIdx);
			if (ParseLeTriShapeData(cursorAt(dataRef), ps.cs, isStrips, bsver > 34)) {
				ps.valid = true;
				shapes.emplace((int32_t)i, std::move(ps));
			} else {
				st.dropped++;
			}
		} else if (t == "BSInvMarker") {
			// The game's own inventory orientation for this item (1/1000 radians)
			ByteCursor b = cursorAt((int32_t)i);
			uint32_t nameIdx = 0;
			uint16_t rx = 0, ry = 0, rz = 0;
			if (b.get(nameIdx) && b.get(rx) && b.get(ry) && b.get(rz)) {
				st.hasInvMarker = true;
				st.invRot[0] = rx / 1000.0f;
				st.invRot[1] = ry / 1000.0f;
				st.invRot[2] = rz / 1000.0f;
			}
		} else if (t == "BSTriShape" || t == "BSMeshLODTriShape") {
			ByteCursor b = cursorAt((int32_t)i);
			PendingShape ps;
			if (!ReadAvBase(b, ps.av, leProps)) { st.dropped++; continue; }
			if (!b.bytes(ps.cs.boundC, 12) || !b.get(ps.cs.boundR)) continue;
			if (!b.get(ps.skinRef) || !b.get(ps.shaderRef) || !b.get(ps.alphaRef)) continue;
			if (!b.get(ps.cs.desc)) continue;
			ps.cs.name = nameOf(ps.av.nameIdx);

			uint32_t vs = DescStride(ps.cs.desc);
			size_t save = b.off;
			uint32_t nt32 = 0, ds = 0;
			uint16_t nv = 0, nt16 = 0;
			bool sized = false;
			uint32_t numTris = 0, numVerts = 0;
			if (b.get(nt32) && b.get(nv) && b.get(ds) && nv && ds == nv * vs + nt32 * 6ull) {
				numTris = nt32;
				numVerts = nv;
				sized = true;
			}
			if (!sized) {
				b.off = save;
				if (b.get(nt16) && b.get(nv) && b.get(ds) && nv && ds == nv * vs + (uint32_t)nt16 * 6ull) {
					numTris = nt16;
					numVerts = nv;
					sized = true;
				}
			}
			if (sized && vs >= 8 && vs <= 64) {
				// Plain shape: vertex block then index block, contiguous
				ps.cs.stride = vs;
				ps.cs.numTris = numTris;
				ps.cs.numVerts = numVerts;
				ps.cs.vertexData.resize((size_t)numVerts * vs);
				ps.cs.indexData.resize((size_t)numTris * 6);
				if (b.bytes(ps.cs.vertexData.data(), ps.cs.vertexData.size()) &&
				    b.bytes(ps.cs.indexData.data(), ps.cs.indexData.size()) &&
				    DecodeLocalAabb(ps.cs.vertexData, ps.cs.desc, vs, numVerts, ps.cs.mn, ps.cs.mx)) {
					ps.valid = true;
				}
			} else if (!sized && DescHasFlag(ps.cs.desc, 0x40) && typeOf(ps.skinRef).size() &&
			           (typeOf(ps.skinRef) == "NiSkinInstance" || typeOf(ps.skinRef) == "BSDismemberSkinInstance")) {
				// Skinned shape: data lives in the NiSkinPartition block
				ByteCursor si = cursorAt(ps.skinRef);
				int32_t dataRef = -1, partRef = -1;
				if (si.get(dataRef) && si.get(partRef) && typeOf(partRef) == "NiSkinPartition") {
					ByteCursor pc = cursorAt(partRef);
					uint32_t nparts = 0, dsz = 0, vsz = 0;
					uint64_t pdesc = 0;
					if (pc.get(nparts) && pc.get(dsz) && pc.get(vsz) && pc.get(pdesc) &&
					    nparts == 1 && vsz >= 8 && vsz <= 64 && dsz && dsz % vsz == 0) {
						uint32_t pnv = dsz / vsz;
						ByteCursor vdata = pc;
						if (pc.skip(dsz) && pnv <= 0xFFFF) {
							std::vector<uint8_t> idx;
							uint32_t triCount = 0;
							// Weight width varies (half vs float); index validation picks the right parse
							bool found = ExtractSkinIndices(data, pc, pnv, 2, idx, triCount) ||
							             ExtractSkinIndices(data, pc, pnv, 4, idx, triCount);
							if (found) {
								ps.cs.desc = pdesc;
								ps.cs.stride = vsz;
								ps.cs.numVerts = pnv;
								ps.cs.numTris = triCount;
								ps.cs.vertexData.resize(dsz);
								ps.cs.indexData = std::move(idx);
								if (vdata.bytes(ps.cs.vertexData.data(), dsz) &&
								    DecodeLocalAabb(ps.cs.vertexData, pdesc, vsz, pnv, ps.cs.mn, ps.cs.mx)) {
									ps.valid = true;
								}
							}
						}
					}
				}
				if (!ps.valid) st.skinnedFail++;
			}
			if (ps.valid) shapes.emplace((int32_t)i, std::move(ps));
			else st.dropped++;
		} else if (t == "BSDynamicTriShape") {
			st.dynamic++;
		} else if (t == "NiAlphaProperty") {
			ByteCursor b = cursorAt((int32_t)i);
			if (ReadObjectNet(b)) {
				uint16_t flags = 0;
				if (b.get(flags)) alphas[(int32_t)i] = { (flags & 1) != 0, ((flags >> 9) & 1) != 0 };
			}
		} else if (t == "BSShaderTextureSet") {
			ByteCursor b = cursorAt((int32_t)i);
			uint32_t n = 0;
			if (b.get(n) && n <= 32) {
				std::vector<std::string> paths(n);
				bool ok = true;
				for (auto& p : paths) {
					uint32_t len;
					if (!b.get(len) || len > 4096) { ok = false; break; }
					p.resize(len);
					if (len && !b.bytes(p.data(), len)) { ok = false; break; }
				}
				if (ok) texSets[(int32_t)i] = std::move(paths);
			}
		}
	}

	// --- Pass 1.5: resolve shader/alpha per shape ---
	for (auto& [idx, ps] : shapes) {
		if (auto ai = alphas.find(ps.alphaRef); ai != alphas.end()) {
			ps.cs.alphaBlend = ai->second.first;
			ps.cs.alphaTest = ai->second.second;
		}
		const std::string& sht = typeOf(ps.shaderRef);
		if (sht == "BSLightingShaderProperty") {
			ByteCursor b = cursorAt(ps.shaderRef);
			uint32_t shaderType = 0;
			if (b.get(shaderType) && ReadObjectNet(b) && b.skip(8 + 16)) {  // flags1+2, uv offset+scale
				int32_t texRef = -1;
				size_t scanBase = b.off;
				if (!b.get(texRef)) texRef = -1;
				if (typeOf(texRef) != "BSShaderTextureSet") {
					// Layout drift fallback: scan nearby dwords for a valid texture-set ref
					texRef = -1;
					for (int k = 0; k < 24 && texRef < 0; ++k) {
						ByteCursor sc{ data.data(), data.size(), scanBase + (size_t)k * 4 };
						int32_t cand = -1;
						if (!sc.get(cand)) break;
						if (typeOf(cand) == "BSShaderTextureSet") texRef = cand;
					}
				}
				if (auto ti = texSets.find(texRef); ti != texSets.end() && !ti->second.empty())
					ps.cs.texPath = ti->second[0];
			}
		} else if (sht == "BSEffectShaderProperty") {
			ps.cs.effect = true;
			ByteCursor b = cursorAt(ps.shaderRef);
			if (ReadObjectNet(b) && b.skip(8 + 16)) {
				uint32_t len = 0;
				if (b.get(len) && len > 0 && len <= 1000) {
					ps.cs.texPath.resize(len);
					if (!b.bytes(ps.cs.texPath.data(), len)) ps.cs.texPath.clear();
				}
			}
		}
	}

	// --- Pass 2: scene traversal accumulating transforms, honoring hidden/Scb/marker skips ---
	auto skipName = [](const std::string& nm) {
		if (nm.size() == 3 && _strnicmp(nm.c_str(), "Scb", 3) == 0) return true;
		if (nm.size() >= 12 && _strnicmp(nm.c_str(), "EditorMarker", 12) == 0) return true;
		return false;
	};
	std::vector<bool> visited(nblocks, false);
	RE::NiTransform identity{};
	if (nodes.count(0) || shapes.count(0)) {
		// Use an explicit work stack so hostile or deeply nested NIFs cannot exhaust
		// the process call stack. Push children in reverse to preserve DFS order.
		std::vector<std::pair<int32_t, RE::NiTransform>> work;
		work.emplace_back(0, identity);
		while (!work.empty()) {
			auto [idx, parent] = std::move(work.back());
			work.pop_back();
			if (idx < 0 || (uint32_t)idx >= nblocks || visited[idx]) continue;
			visited[idx] = true;

			if (auto ni = nodes.find(idx); ni != nodes.end()) {
				auto& nr = ni->second;
				if (nr.av.flags & 1) continue;  // hidden
				if (skipName(nameOf(nr.av.nameIdx))) continue;
				RE::NiTransform xfw = parent * nr.av.xf;
				for (auto ch = nr.children.rbegin(); ch != nr.children.rend(); ++ch)
					work.emplace_back(*ch, xfw);
				continue;
			}

			if (auto si = shapes.find(idx); si != shapes.end()) {
				auto& ps = si->second;
				if (ps.av.flags & 1) continue;
				if (skipName(ps.cs.name)) continue;
				ps.cs.world = parent * ps.av.xf;
				out.push_back(std::move(ps.cs));
				st.shapes++;
				if (out.back().effect) st.fx++;
			}
		}
	} else {
		// Unusual root layout: emit every parsed shape with its local transform
		for (auto& [idx, ps] : shapes) {
			if (ps.av.flags & 1 || skipName(ps.cs.name)) continue;
			ps.cs.world = ps.av.xf;
			out.push_back(std::move(ps.cs));
			st.shapes++;
			if (out.back().effect) st.fx++;
		}
	}
	return !out.empty();
}

// ============================================================================
// Section D: self-contained GPU model (no engine loader, worker-thread safe)
// ============================================================================

struct BoundSphere {
	XMFLOAT3 c = { 0, 0, 0 };
	float r = 0;
};

struct ExtBox {
	float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
	float mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
	bool any = false;

	void Extend(const RE::NiPoint3& p)
	{
		const float c[3] = { p.x, p.y, p.z };
		for (int k = 0; k < 3; ++k) {
			mn[k] = (c[k] < mn[k]) ? c[k] : mn[k];
			mx[k] = (c[k] > mx[k]) ? c[k] : mx[k];
		}
		any = true;
	}
};

static void MergeSphere(BoundSphere& acc, const XMFLOAT3& c, float r)
{
	if (acc.r <= 0) {
		acc = { c, r };
		return;
	}
	float dx = c.x - acc.c.x, dy = c.y - acc.c.y, dz = c.z - acc.c.z;
	float d = sqrtf(dx * dx + dy * dy + dz * dz);
	if (acc.r >= d + r) return;
	if (r >= d + acc.r) { acc = { c, r }; return; }
	float nr = (d + acc.r + r) * 0.5f;
	float t = (nr - acc.r) / d;
	acc.c = { acc.c.x + dx * t, acc.c.y + dy * t, acc.c.z + dz * t };
	acc.r = nr;
}

static void NiToWorldMatrix(const RE::NiTransform& xf, XMFLOAT4X4& out)
{
	// Row-vector convention: M[j][i] = scale * R[i][j], row 3 = translation
	const auto& R = xf.rotate.entry;
	float s = xf.scale;
	out = XMFLOAT4X4(
		s * R[0][0], s * R[1][0], s * R[2][0], 0,
		s * R[0][1], s * R[1][1], s * R[2][1], 0,
		s * R[0][2], s * R[1][2], s * R[2][2], 0,
		xf.translate.x, xf.translate.y, xf.translate.z, 1);
}

struct GpuItem {
	ComPtr<ID3D11Buffer> vb, ib;
	ComPtr<ID3D11ShaderResourceView> srv;  // null: gray fallback (lit) or skip (effect)
	UINT stride = 0, indexCount = 0;
	uint64_t desc = 0;
	int vsIndex = 0;
	bool floatPos = false;
	bool alphaTest = false, alphaBlend = false, effect = false;
	XMFLOAT4X4 world;
};

struct PreviewModel {
	std::vector<GpuItem> items;
	BoundSphere bound, fxBound;
	ExtBox box;  // non-effect extents in model space
	int drawn = 0, fx = 0;
	bool hasInvMarker = false;  // BSInvMarker pose from the file (game inventory orientation)
	float invRot[3] = {};
};

// DDS texture cache, worker-thread only (single worker; cleared at Shutdown after join)
static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> g_texCache;
static std::atomic<ID3D11Device*> g_devForWorker{ nullptr };

static ComPtr<ID3D11ShaderResourceView> LoadTextureCached(ID3D11Device* dev, const std::string& rawPath)
{
	std::string path = LowerStr(rawPath.c_str());
	for (auto& ch : path)
		if (ch == '/') ch = '\\';
	if (path.rfind("textures\\", 0) != 0) path = "textures\\" + path;

	if (auto it = g_texCache.find(path); it != g_texCache.end()) return it->second;
	if (g_texCache.size() > 96) g_texCache.clear();

	ComPtr<ID3D11ShaderResourceView> srv;
	std::vector<uint8_t> dds;
	if (ReadGameFile(path, dds)) {
		ComPtr<ID3D11Resource> res;
		if (FAILED(DirectX::CreateDDSTextureFromMemory(dev, dds.data(), dds.size(), &res, &srv)))
			srv.Reset();
	}
	if (!srv) logger::debug("ModelPreview: texture missing '{}'", path);
	g_texCache[path] = srv;
	return srv;
}

static std::shared_ptr<PreviewModel> BuildGpuModel(ID3D11Device* dev, std::vector<CpuShape>& shapes)
{
	auto pm = std::make_shared<PreviewModel>();
	for (auto& s : shapes) {
		if (!DescHasFlag(s.desc, 0x1)) continue;  // no position stream
		GpuItem it;

		D3D11_BUFFER_DESC bd = {};
		bd.Usage = D3D11_USAGE_IMMUTABLE;
		bd.ByteWidth = (UINT)s.vertexData.size();
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA init = { s.vertexData.data(), 0, 0 };
		if (FAILED(dev->CreateBuffer(&bd, &init, &it.vb))) continue;
		bd.ByteWidth = (UINT)s.indexData.size();
		bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
		init.pSysMem = s.indexData.data();
		if (FAILED(dev->CreateBuffer(&bd, &init, &it.ib))) continue;

		if (!s.texPath.empty()) it.srv = LoadTextureCached(dev, s.texPath);
		it.stride = s.stride;
		it.indexCount = s.numTris * 3;
		it.desc = s.desc;
		it.vsIndex = (DescHasFlag(s.desc, 0x2) ? 1 : 0) | (DescHasFlag(s.desc, 0x8) ? 2 : 0);
		it.floatPos = DescPosBytes(s.desc) >= 16;
		it.alphaTest = s.alphaTest;
		it.alphaBlend = s.alphaBlend;
		it.effect = s.effect;
		NiToWorldMatrix(s.world, it.world);

		// Bounds: world AABB from the decoded local box; sphere from file value or box
		RE::NiPoint3 corners[8];
		ExtBox wbox;
		for (int corner = 0; corner < 8; ++corner) {
			RE::NiPoint3 lp{ (corner & 1) ? s.mx[0] : s.mn[0],
				(corner & 2) ? s.mx[1] : s.mn[1],
				(corner & 4) ? s.mx[2] : s.mn[2] };
			corners[corner] = s.world * lp;
			wbox.Extend(corners[corner]);
		}
		XMFLOAT3 sc;
		float sr;
		if (s.boundR > 0.01f) {
			RE::NiPoint3 wc = s.world * RE::NiPoint3{ s.boundC[0], s.boundC[1], s.boundC[2] };
			sc = { wc.x, wc.y, wc.z };
			sr = s.boundR * s.world.scale;
		} else {
			float ex = wbox.mx[0] - wbox.mn[0], ey = wbox.mx[1] - wbox.mn[1], ez = wbox.mx[2] - wbox.mn[2];
			sc = { (wbox.mn[0] + wbox.mx[0]) * 0.5f, (wbox.mn[1] + wbox.mx[1]) * 0.5f, (wbox.mn[2] + wbox.mx[2]) * 0.5f };
			sr = 0.5f * sqrtf(ex * ex + ey * ey + ez * ez);
		}
		if (s.effect) {
			MergeSphere(pm->fxBound, sc, sr);
			pm->fx++;
		} else {
			MergeSphere(pm->bound, sc, sr);
			for (auto& wc : corners) pm->box.Extend(wc);
			pm->drawn++;
		}
		pm->items.push_back(std::move(it));
	}
	return pm->items.empty() ? nullptr : pm;
}

static std::string ResolveModelPath(const Request& req, int& kindOut)
{
	kindOut = kKindGeneric;
	RE::TESForm* form = nullptr;
	// Exact runtime FormID is authoritative; LookupForm(localId, plugin) can re-derive a different form (injected/cross-master IDs).
	if (req.formId) {
		form = RE::TESForm::LookupByID(req.formId);
	}
	if (!form && !req.plugin.empty()) {
		if (auto dh = RE::TESDataHandler::GetSingleton())
			form = dh->LookupForm(req.localId, req.plugin);
	}
	if (!form) {
		logger::warn("ModelPreview: form not found ({} {:08X} fid {:08X})", req.plugin, req.localId, req.formId);
		return "";
	}

	// Weapons: an enchanted/named variant ("fine greatsword", enchanted staves/scepters)
	// often has a blank model and inherits it from a base weapon via CNAM (Use Template).
	// Read the own model first (normal weapons), then walk the template chain if empty.
	if (auto* weap = form->As<RE::TESObjectWEAP>()) {
		kindOut = kKindElongate;
		RE::TESObjectWEAP* t = weap;
		for (int guard = 0; t && guard < 8; ++guard) {
			if (auto* tm = skyrim_cast<RE::TESModel*>(t)) {
				const char* m = tm->GetModel();
				if (m && m[0]) return m;
			}
			t = t->templateWeapon;
		}
		logger::info("ModelPreview: weapon {:08X} has no model (own + template chain empty)", form->GetFormID());
		return "";
	}
	if (form->Is(RE::FormType::Ammo)) kindOut = kKindAmmo;

	// Armor uses the ground/world model, not the biped model
	if (auto armo = form->As<RE::TESObjectARMO>()) {
		if (armo->HasPartOf(RE::BIPED_MODEL::BipedObjectSlot::kShield))
			kindOut = kKindShield;
		else if (armo->GetArmorType() == RE::TESObjectARMO::ArmorType::kClothing)
			kindOut = kKindClothing;
		else
			kindOut = kKindArmor;
		const char* m = armo->worldModels[0].GetModel();
		if (!m || !m[0]) m = armo->worldModels[1].GetModel();
		return (m && m[0]) ? m : "";
	}

	if (auto mdl = skyrim_cast<RE::TESModel*>(form)) {
		const char* m = mdl->GetModel();
		if (!m || !m[0]) {
			logger::info("ModelPreview: form {:08X} has an empty model path (no world model)", form->GetFormID());
			return "";
		}
		return m;
	}

	logger::warn("ModelPreview: form {:08X} has no model component", form->GetFormID());
	return "";
}

// LRU cache of built models, worker-thread only
static std::list<std::pair<std::string, std::shared_ptr<PreviewModel>>> g_lru;
static constexpr size_t LRU_MAX = 24;

static std::shared_ptr<PreviewModel> LoadModel(ID3D11Device* dev, const std::string& path)
{
	std::string key = LowerStr(path.c_str());
	for (auto it = g_lru.begin(); it != g_lru.end(); ++it) {
		if (it->first == key) {
			g_lru.splice(g_lru.begin(), g_lru, it);
			return g_lru.front().second;
		}
	}

	auto t0 = std::chrono::steady_clock::now();
	std::vector<uint8_t> data;
	if (!ReadGameFile("meshes\\" + path, data) && !ReadGameFile(path, data)) {
		logger::warn("ModelPreview: model file not found '{}'", path);
		return nullptr;
	}
	std::vector<CpuShape> shapes;
	ParseStats st;
	std::shared_ptr<PreviewModel> pm;
	if (ParseNifModel(data, shapes, st)) pm = BuildGpuModel(dev, shapes);
	if (pm && st.hasInvMarker) {
		pm->hasInvMarker = true;
		memcpy(pm->invRot, st.invRot, sizeof(pm->invRot));
	}
	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
	logger::info("ModelPreview: load '{}' shapes={} fx={} skinnedFail={} dyn={} dropped={} ok={} ({} ms)",
		path, st.shapes, st.fx, st.skinnedFail, st.dynamic, st.dropped, pm != nullptr, ms);

	if (pm) {
		g_lru.emplace_front(std::move(key), pm);
		while (g_lru.size() > LRU_MAX) g_lru.pop_back();
	}
	return pm;
}

// Worker thread: file IO + parse + D3D resource creation only - no engine loader,
// so off-main-thread is safe (no Demand deadlocks, no main-thread hitch)
struct Job {
	std::string fullKey;
	uint64_t loadGen = 0;
	Request req;
};
static std::mutex g_jobMutex;
static std::condition_variable g_jobCv;
static std::deque<Job> g_jobs;  // FIFO; a grid queues many distinct items at once
static std::thread g_worker;
static bool g_workerStarted = false;
static std::atomic<bool> g_workerExit{ false };

static void WorkerMain()
{
	while (true) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(g_jobMutex);
			g_jobCv.wait(lock, [] { return g_workerExit.load() || !g_jobs.empty(); });
			if (g_workerExit.load()) return;
			job = std::move(g_jobs.front());
			g_jobs.pop_front();
		}

		LoadResult res;
		res.fullKey = job.fullKey;
		res.loadGen = job.loadGen;
		res.key = MakeKey(job.req);
		try {
			ID3D11Device* dev = g_devForWorker.load();
			std::string path = ResolveModelPath(job.req, res.kind);
			if (!path.empty() && dev) {
				res.model = LoadModel(dev, path);
				res.ok = res.model != nullptr;
			}
		} catch (...) {
			logger::error("ModelPreview: exception during model load");
			res.ok = false;
			res.model = nullptr;
		}

		{
			std::lock_guard<std::mutex> lock(g_resultMutex);
			g_results.push_back(std::move(res));
		}
	}
}

static void PostJob(const std::string& fullKey, uint64_t loadGen, const Request& req)
{
	{
		std::lock_guard<std::mutex> lock(g_jobMutex);
		if (!g_workerStarted) {
			g_worker = std::thread(WorkerMain);
			g_workerStarted = true;
		}
		g_jobs.push_back(Job{ fullKey, loadGen, req });
	}
	g_jobCv.notify_one();
}

// ============================================================================
// Section E: D3D resources (render thread only)
// ============================================================================

struct CBData {
	XMFLOAT4X4 wvp;
	XMFLOAT4X4 world;
	XMFLOAT4 light0;  // xyz = travel direction, w = intensity
	XMFLOAT4 light1;
	XMFLOAT4 params;  // x = alpha-test, y = alpha-blend floor, z = effect/unlit
};
static_assert(sizeof(CBData) % 16 == 0);

static const char* SHADER_SRC = R"hlsl(
cbuffer CB : register(b0) {
    float4x4 gWVP;
    float4x4 gWorld;
    float4 gLight0;
    float4 gLight1;
    float4 gParams;
};
Texture2D gDiffuse : register(t0);
SamplerState gSamp : register(s0);

struct VSIn {
    float4 pos : POSITION;
#ifdef HAS_UV
    float2 uv : TEXCOORD0;
#endif
#ifdef HAS_NORMAL
    float4 nrm : NORMAL;
#endif
};
struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 nrm : NORMAL0;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos.xyz, 1.0), gWVP);
#ifdef HAS_UV
    o.uv = i.uv;
#else
    o.uv = float2(0.5, 0.5);
#endif
#ifdef HAS_NORMAL
    o.nrm = normalize(mul(i.nrm.xyz * 2.0 - 1.0, (float3x3)gWorld));
#else
    o.nrm = float3(0.0, -1.0, 0.3);
#endif
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float4 tex = gDiffuse.Sample(gSamp, i.uv);
    if (gParams.z > 0.5) {
        // Effect glow: additive, alpha follows brightness so dark texels add nothing
        float glow = dot(tex.rgb, float3(0.299, 0.587, 0.114));
        return float4(tex.rgb, glow * tex.a);
    }
    clip(gParams.x > 0.5 ? tex.a - 0.5 : 1.0);
    float3 n = normalize(i.nrm);
    float bright = gParams.w;  // user brightness (1.0 = default)
    float lit = (0.48 + saturate(dot(n, -gLight0.xyz)) * gLight0.w
                      + saturate(dot(n, -gLight1.xyz)) * gLight1.w) * bright;
    // Museum rim light: bright edge where the surface turns away from the camera
    float3 viewDir = float3(0.0, -0.82, 0.57);
    float rim = pow(1.0 - saturate(dot(n, viewDir)), 2.5) * 0.45 * bright;
    float a = gParams.y > 0.5 ? max(tex.a, 0.4) : 1.0;
    return float4(tex.rgb * lit + rim * float3(0.9, 0.95, 1.0), a);
}
)hlsl";

static ID3D11Device* g_dev = nullptr;
static bool g_d3dReady = false;
static bool g_d3dFailed = false;

static ComPtr<ID3D11Texture2D> g_depthTex;   // shared depth, previews render sequentially
static ComPtr<ID3D11DepthStencilView> g_dsv;
static ComPtr<ID3D11Buffer> g_cb;
static ComPtr<ID3D11SamplerState> g_sampler;
static ComPtr<ID3D11RasterizerState> g_rasterizer;
static ComPtr<ID3D11DepthStencilState> g_depthState;
static ComPtr<ID3D11DepthStencilState> g_depthStateNoWrite;
static ComPtr<ID3D11BlendState> g_blendState;
static ComPtr<ID3D11BlendState> g_blendStateAlpha;
static ComPtr<ID3D11BlendState> g_blendStateAdditive;
static ComPtr<ID3D11ShaderResourceView> g_graySrv;
static ComPtr<ID3D11PixelShader> g_ps;
static ComPtr<ID3D11VertexShader> g_vs[4];  // index = HAS_UV | HAS_NORMAL<<1
static ComPtr<ID3DBlob> g_vsBlob[4];
static std::map<uint64_t, ComPtr<ID3D11InputLayout>> g_layouts;
static std::mutex g_layoutMutex;  // layouts touched by render thread only, but cheap insurance

static bool CompileShaders()
{
	UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL2;
	for (int i = 0; i < 4; ++i) {
		D3D_SHADER_MACRO macros[3] = {};
		int n = 0;
		if (i & 1) macros[n++] = { "HAS_UV", "1" };
		if (i & 2) macros[n++] = { "HAS_NORMAL", "1" };
		ComPtr<ID3DBlob> errors;
		HRESULT hr = D3DCompile(SHADER_SRC, strlen(SHADER_SRC), "ModelPreview", macros, nullptr,
			"VSMain", "vs_5_0", flags, 0, &g_vsBlob[i], &errors);
		if (FAILED(hr)) {
			logger::error("ModelPreview: VS compile failed ({}): {}", i,
				errors ? (const char*)errors->GetBufferPointer() : "?");
			return false;
		}
		hr = g_dev->CreateVertexShader(g_vsBlob[i]->GetBufferPointer(), g_vsBlob[i]->GetBufferSize(), nullptr, &g_vs[i]);
		if (FAILED(hr)) return false;
	}

	ComPtr<ID3DBlob> psBlob, errors;
	HRESULT hr = D3DCompile(SHADER_SRC, strlen(SHADER_SRC), "ModelPreview", nullptr, nullptr,
		"PSMain", "ps_5_0", flags, 0, &psBlob, &errors);
	if (FAILED(hr)) {
		logger::error("ModelPreview: PS compile failed: {}",
			errors ? (const char*)errors->GetBufferPointer() : "?");
		return false;
	}
	return SUCCEEDED(g_dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &g_ps));
}

static bool EnsureRenderResources(ID3D11Device* dev)
{
	if (g_d3dReady) return true;
	if (g_d3dFailed) return false;
	g_dev = dev;

	auto fail = [](const char* what) {
		logger::error("ModelPreview: D3D init failed at {}", what);
		g_d3dFailed = true;
		return false;
	};

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = td.Height = (UINT)g_cfg.rtSize;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc = { 1, 0 };
	td.Usage = D3D11_USAGE_DEFAULT;
	td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_depthTex))) return fail("depth tex");
	if (FAILED(dev->CreateDepthStencilView(g_depthTex.Get(), nullptr, &g_dsv))) return fail("dsv");

	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(CBData);
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb))) return fail("cb");

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(dev->CreateSamplerState(&sd, &g_sampler))) return fail("sampler");

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	if (FAILED(dev->CreateRasterizerState(&rd, &g_rasterizer))) return fail("rasterizer");

	D3D11_DEPTH_STENCIL_DESC dsd = {};
	dsd.DepthEnable = TRUE;
	dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	dsd.DepthFunc = D3D11_COMPARISON_LESS;
	if (FAILED(dev->CreateDepthStencilState(&dsd, &g_depthState))) return fail("depth state");
	dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	if (FAILED(dev->CreateDepthStencilState(&dsd, &g_depthStateNoWrite))) return fail("depth state nw");

	D3D11_BLEND_DESC bld = {};
	bld.RenderTarget[0].BlendEnable = FALSE;
	bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(dev->CreateBlendState(&bld, &g_blendState))) return fail("blend state");
	bld.RenderTarget[0].BlendEnable = TRUE;
	bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	if (FAILED(dev->CreateBlendState(&bld, &g_blendStateAlpha))) return fail("alpha blend state");
	bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
	bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
	bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	if (FAILED(dev->CreateBlendState(&bld, &g_blendStateAdditive))) return fail("additive blend state");

	uint32_t gray = 0xFF808080;
	D3D11_TEXTURE2D_DESC gd = {};
	gd.Width = gd.Height = 1;
	gd.MipLevels = 1;
	gd.ArraySize = 1;
	gd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	gd.SampleDesc = { 1, 0 };
	gd.Usage = D3D11_USAGE_IMMUTABLE;
	gd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA gi = { &gray, 4, 0 };
	ComPtr<ID3D11Texture2D> grayTex;
	if (FAILED(dev->CreateTexture2D(&gd, &gi, &grayTex))) return fail("gray tex");
	if (FAILED(dev->CreateShaderResourceView(grayTex.Get(), nullptr, &g_graySrv))) return fail("gray srv");

	if (!CompileShaders()) {
		g_d3dFailed = true;
		return false;
	}

	g_d3dReady = true;
	logger::info("ModelPreview: D3D resources ready ({}x{})", g_cfg.rtSize, g_cfg.rtSize);
	return true;
}

static ID3D11InputLayout* GetOrCreateLayout(uint64_t desc, int vsIndex, bool floatPos)
{
	std::lock_guard<std::mutex> lock(g_layoutMutex);
	auto it = g_layouts.find(desc);
	if (it != g_layouts.end()) return it->second.Get();

	std::vector<D3D11_INPUT_ELEMENT_DESC> elems;
	elems.push_back({ "POSITION", 0, floatPos ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT,
		0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 });
	if (vsIndex & 1)
		elems.push_back({ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT,
			0, DescAttrOffset(desc, 1), D3D11_INPUT_PER_VERTEX_DATA, 0 });
	if (vsIndex & 2)
		elems.push_back({ "NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM,
			0, DescAttrOffset(desc, 3), D3D11_INPUT_PER_VERTEX_DATA, 0 });

	ComPtr<ID3D11InputLayout> layout;
	HRESULT hr = g_dev->CreateInputLayout(elems.data(), (UINT)elems.size(),
		g_vsBlob[vsIndex]->GetBufferPointer(), g_vsBlob[vsIndex]->GetBufferSize(), &layout);
	if (FAILED(hr)) {
		logger::warn("ModelPreview: CreateInputLayout failed for desc {:016X}", desc);
		g_layouts[desc] = nullptr;
		return nullptr;
	}
	g_layouts[desc] = layout;
	return layout.Get();
}

// ============================================================================
// Section F: per-preview GPU target + pose
// ============================================================================

// Each preview owns a color target at iRTSize; the depth buffer is shared because
// previews render one at a time. Created lazily, reused for the preview's lifetime.
static bool EnsurePreviewTarget(Preview& pv)
{
	if (pv.tex) return true;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = td.Height = (UINT)g_cfg.rtSize;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	td.SampleDesc = { 1, 0 };
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &pv.tex))) return false;
	if (FAILED(g_dev->CreateRenderTargetView(pv.tex.Get(), nullptr, &pv.rtv))) return false;
	if (FAILED(g_dev->CreateShaderResourceView(pv.tex.Get(), nullptr, &pv.srv))) return false;
	pv.dirty = true;
	return true;
}

// Compute framing + pose for a freshly loaded model and attach it to the preview.
static bool ComputePose(Preview& pv, std::shared_ptr<PreviewModel> pm, const std::string& key, int kind)
{
	if (!pm || pm->items.empty()) return false;
	pv.model = pm;
	pv.modelKey = key;
	pv.kind = kind;
	XMStoreFloat4x4(&pv.orient, XMMatrixIdentity());

	BoundSphere bound = pm->bound;
	if (bound.r <= 0.001f && pm->fxBound.r > 0.001f) bound = pm->fxBound;  // all-effect items
	pv.center = bound.c;
	pv.radius = (bound.r > 0.001f) ? bound.r : 1.0f;

	// Exact extents always exist now (decoded straight from file vertex data)
	float ex = 0, ey = 0, ez = 0;
	bool haveExt = pm->box.any;
	if (haveExt) {
		ex = pm->box.mx[0] - pm->box.mn[0];
		ey = pm->box.mx[1] - pm->box.mn[1];
		ez = pm->box.mx[2] - pm->box.mn[2];
		XMFLOAT3 mid = { (pm->box.mn[0] + pm->box.mx[0]) * 0.5f, (pm->box.mn[1] + pm->box.mx[1]) * 0.5f,
			(pm->box.mn[2] + pm->box.mx[2]) * 0.5f };
		float extR = 0.5f * sqrtf(ex * ex + ey * ey + ez * ez);
		if (extR > 0.001f && (pv.radius < 0.25f * extR || pv.radius > 4.0f * extR)) {
			pv.center = mid;
			pv.radius = extR;
		}
	}

	// Pose by kind: weapons stand long axis up; shields raise; armor/clothing use the
	// mesh BSInvMarker (inverse) when present; everything else stays as authored.
	const char* orientSrc = "asis";
	if ((kind == kKindElongate || kind == kKindAmmo) && haveExt) {
		XMMATRIX o = XMMatrixIdentity();
		bool stood = false;
		if (ex >= ey && ex >= ez && ex > 1.2f * ez) { o = XMMatrixRotationY(-XM_PIDIV2); stood = true; }
		else if (ey >= ex && ey >= ez && ey > 1.2f * ez) { o = XMMatrixRotationX(XM_PIDIV2); stood = true; }
		if (kind == kKindAmmo) o = o * XMMatrixRotationX(XM_PI);  // quivers stand tip-down
		if (stood || kind == kKindAmmo) { XMStoreFloat4x4(&pv.orient, o); orientSrc = "stood"; }
	} else if (kind == kKindShield) {
		XMStoreFloat4x4(&pv.orient, XMMatrixRotationX(XM_PIDIV2));
		orientSrc = "shield";
	} else if (kind == kKindArmor || kind == kKindClothing) {
		if (pm->hasInvMarker) {
			// Marker stores the view rotation; apply the inverse to the object.
			XMMATRIX o = XMMatrixRotationZ(-pm->invRot[2])
			           * XMMatrixRotationY(-pm->invRot[1])
			           * XMMatrixRotationX(-pm->invRot[0]);
			XMStoreFloat4x4(&pv.orient, o);
			orientSrc = "invmarker";
		} else if (haveExt) {
			float planar = (ex > ey) ? ex : ey;
			if (planar > 1.35f * ez) {
				float sign = (kind == kKindClothing) ? XM_PIDIV2 : -XM_PIDIV2;
				XMStoreFloat4x4(&pv.orient, XMMatrixRotationX(sign));
				orientSrc = (kind == kKindClothing) ? "clothing" : "armor";
			}
		}
	}

	pv.dirty = true;
	logger::info("ModelPreview: '{}' drawn={} fx={} radius={:.1f} orient={}",
		key, pm->drawn, pm->fx, pv.radius, orientSrc);
	return true;
}

// ============================================================================
// Section G: render pass
// ============================================================================

// ImGui-style full pipeline state backup/restore around our pass
struct ScopedStateBackup {
	ID3D11DeviceContext* ctx;
	ID3D11InputLayout* layout = nullptr;
	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	ID3D11Buffer* vb = nullptr;
	UINT vbStride = 0, vbOffset = 0;
	ID3D11Buffer* ib = nullptr;
	DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
	UINT ibOffset = 0;
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	ID3D11Buffer* vsCb = nullptr;
	ID3D11Buffer* psCb = nullptr;
	ID3D11ShaderResourceView* psSrv = nullptr;
	ID3D11SamplerState* psSamp = nullptr;
	ID3D11RasterizerState* rs = nullptr;
	UINT numViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	ID3D11BlendState* blend = nullptr;
	FLOAT blendFactor[4] = {};
	UINT sampleMask = 0;
	ID3D11DepthStencilState* dss = nullptr;
	UINT stencilRef = 0;
	ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* dsv = nullptr;

	explicit ScopedStateBackup(ID3D11DeviceContext* c) : ctx(c)
	{
		ctx->IAGetInputLayout(&layout);
		ctx->IAGetPrimitiveTopology(&topology);
		ctx->IAGetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
		ctx->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
		ctx->VSGetShader(&vs, nullptr, nullptr);
		ctx->PSGetShader(&ps, nullptr, nullptr);
		ctx->VSGetConstantBuffers(0, 1, &vsCb);
		ctx->PSGetConstantBuffers(0, 1, &psCb);
		ctx->PSGetShaderResources(0, 1, &psSrv);
		ctx->PSGetSamplers(0, 1, &psSamp);
		ctx->RSGetState(&rs);
		ctx->RSGetViewports(&numViewports, viewports);
		ctx->OMGetBlendState(&blend, blendFactor, &sampleMask);
		ctx->OMGetDepthStencilState(&dss, &stencilRef);
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
	}

	~ScopedStateBackup()
	{
		ctx->IASetInputLayout(layout);
		ctx->IASetPrimitiveTopology(topology);
		ctx->IASetVertexBuffers(0, 1, &vb, &vbStride, &vbOffset);
		ctx->IASetIndexBuffer(ib, ibFormat, ibOffset);
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->PSSetShader(ps, nullptr, 0);
		ctx->VSSetConstantBuffers(0, 1, &vsCb);
		ctx->PSSetConstantBuffers(0, 1, &psCb);
		ctx->PSSetShaderResources(0, 1, &psSrv);
		ctx->PSSetSamplers(0, 1, &psSamp);
		ctx->RSSetState(rs);
		if (numViewports) ctx->RSSetViewports(numViewports, viewports);
		ctx->OMSetBlendState(blend, blendFactor, sampleMask);
		ctx->OMSetDepthStencilState(dss, stencilRef);
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, dsv);

		auto rel = [](IUnknown* p) { if (p) p->Release(); };
		rel(layout); rel(vb); rel(ib); rel(vs); rel(ps); rel(vsCb); rel(psCb);
		rel(psSrv); rel(psSamp); rel(rs); rel(blend); rel(dss); rel(dsv);
		for (auto* r : rtvs) rel(r);
	}
};

static float EffSpin(float s) { return (s > -9999.0f) ? s : g_cfg.spinDegPerSec; }

static void DumpRTOnce(ID3D11DeviceContext* ctx, Preview& pv);

// Render one preview into its own color target. The caller wraps the whole batch in
// a single ScopedStateBackup; the shared depth buffer is cleared per preview.
static void RenderPreviewInner(ID3D11DeviceContext* ctx, Preview& pv)
{
	if (!pv.model || !pv.rtv) return;

	if (!pv.spinStarted) {
		pv.spinStart = std::chrono::steady_clock::now();
		pv.spinStarted = true;
	}
	double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - pv.spinStart).count();
	float spinRate = EffSpin(pv.spin);
	float angle = XMConvertToRadians(pv.yaw) + (float)(elapsed * spinRate * (XM_PI / 180.0));
	pv.lastAngleDeg = angle * (180.0f / XM_PI);

	const float clearColor[4] = { 0, 0, 0, 0 };
	ctx->ClearRenderTargetView(pv.rtv.Get(), clearColor);
	ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

	ID3D11RenderTargetView* rtv = pv.rtv.Get();
	ctx->OMSetRenderTargets(1, &rtv, g_dsv.Get());
	D3D11_VIEWPORT vp = { 0, 0, (float)g_cfg.rtSize, (float)g_cfg.rtSize, 0, 1 };
	ctx->RSSetViewports(1, &vp);
	ctx->RSSetState(g_rasterizer.Get());
	const float bf[4] = { 0, 0, 0, 0 };
	ctx->OMSetBlendState(g_blendState.Get(), bf, 0xFFFFFFFF);
	ctx->OMSetDepthStencilState(g_depthState.Get(), 0);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11SamplerState* samp = g_sampler.Get();
	ctx->PSSetSamplers(0, 1, &samp);
	ID3D11Buffer* cb = g_cb.Get();
	ctx->VSSetConstantBuffers(0, 1, &cb);
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ctx->PSSetShader(g_ps.Get(), nullptr, 0);

	// Camera: Z-up showcase angle (~35 deg above), looking at the origin-centered model
	float R = pv.radius;
	float d = R / tanf(XMConvertToRadians(15.0f)) * 1.12f / pv.zoom;
	float elev = XMConvertToRadians(35.0f);
	XMVECTOR panOfs = XMVectorSet(pv.panX * R, 0, pv.panY * R, 0);
	XMVECTOR eye = XMVectorAdd(XMVectorSet(0, -d * cosf(elev), d * sinf(elev), 1), panOfs);
	XMVECTOR at = XMVectorAdd(XMVectorSet(0, 0, 0, 1), panOfs);
	XMVECTOR up = XMVectorSet(0, 0, 1, 0);
	XMMATRIX view = XMMatrixLookAtRH(eye, at, up);
	XMMATRIX proj = XMMatrixPerspectiveFovRH(XMConvertToRadians(30.0f), 1.0f, 0.05f * R, d + 4.0f * R);
	XMMATRIX recenter = XMMatrixTranslation(-pv.center.x, -pv.center.y, -pv.center.z);
	XMMATRIX orient = XMLoadFloat4x4(&pv.orient);
	if (pv.flip) orient = orient * XMMatrixRotationX(XM_PI);
	if (pv.roll != 0.0f) orient = orient * XMMatrixRotationY(XMConvertToRadians(pv.roll));
	XMMATRIX spin = XMMatrixRotationZ(angle);
	if (pv.pitch != 0.0f) spin = spin * XMMatrixRotationX(XMConvertToRadians(pv.pitch));

	XMVECTOR l0 = XMVector3Normalize(XMVectorSet(0.45f, 0.80f, -0.50f, 0));
	XMVECTOR l1 = XMVector3Normalize(XMVectorSet(-0.60f, 0.40f, 0.25f, 0));

	auto drawItem = [&](const GpuItem& item) {
		ID3D11InputLayout* layout = GetOrCreateLayout(item.desc, item.vsIndex, item.floatPos);
		if (!layout) return;

		ID3D11ShaderResourceView* srv = item.srv.Get();
		if (!srv) {
			if (item.effect) return;  // untextured glow: skip, no gray disks
			srv = g_graySrv.Get();
		}

		XMMATRIX world = XMLoadFloat4x4(&item.world) * recenter * orient * spin;
		XMMATRIX wvp = world * view * proj;

		D3D11_MAPPED_SUBRESOURCE mapped;
		if (FAILED(ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
		auto* data = (CBData*)mapped.pData;
		XMStoreFloat4x4(&data->wvp, XMMatrixTranspose(wvp));
		XMStoreFloat4x4(&data->world, XMMatrixTranspose(world));
		XMStoreFloat4(&data->light0, XMVectorSetW(l0, 1.1f));
		XMStoreFloat4(&data->light1, XMVectorSetW(l1, 0.5f));
		data->params = { item.alphaTest ? 1.0f : 0.0f, item.alphaBlend ? 1.0f : 0.0f, item.effect ? 1.0f : 0.0f, pv.brightness };
		ctx->Unmap(g_cb.Get(), 0);

		ctx->PSSetShaderResources(0, 1, &srv);
		ctx->IASetInputLayout(layout);
		ctx->VSSetShader(g_vs[item.vsIndex].Get(), nullptr, 0);
		UINT offset = 0;
		ID3D11Buffer* vb = item.vb.Get();
		ctx->IASetVertexBuffers(0, 1, &vb, &item.stride, &offset);
		ctx->IASetIndexBuffer(item.ib.Get(), DXGI_FORMAT_R16_UINT, 0);
		ctx->DrawIndexed(item.indexCount, 0, 0);
	};

	// Opaque pass, then blended glass, then additive effect glows (depth read only)
	auto& items = pv.model->items;
	for (auto& item : items)
		if (!item.alphaBlend && !item.effect) drawItem(item);
	ctx->OMSetBlendState(g_blendStateAlpha.Get(), bf, 0xFFFFFFFF);
	ctx->OMSetDepthStencilState(g_depthStateNoWrite.Get(), 0);
	for (auto& item : items)
		if (item.alphaBlend && !item.effect) drawItem(item);
	ctx->OMSetBlendState(g_blendStateAdditive.Get(), bf, 0xFFFFFFFF);
	for (auto& item : items)
		if (item.effect) drawItem(item);

	if (g_cfg.dumpRT) DumpRTOnce(ctx, pv);
}

// ============================================================================
// Section H: flat presenter
// ============================================================================

void GetFlatOverlays(uint64_t viewId, std::vector<FlatDraw>& out)
{
	if (!g_d3dReady || viewId == 0) return;
	for (auto& [key, pv] : g_previews) {
		if (pv.viewId != viewId || !pv.model || !pv.srv) continue;
		RECT dest;
		dest.left = (LONG)pv.rect.x;
		dest.top = (LONG)pv.rect.y;
		dest.right = (LONG)(pv.rect.x + pv.rect.w);
		dest.bottom = (LONG)(pv.rect.y + pv.rect.h);
		out.push_back({ pv.srv.Get(), dest });
	}
}

// ============================================================================
// Section I: VR presenter (one overlay per preview)
// ============================================================================

static std::atomic<void*> g_cachedOverlayIface{ nullptr };
static constexpr int MAX_VR_PREVIEWS = 32;  // OpenVR overlay budget guard

static void DestroyPreviewOverlay(Preview& pv)
{
	void* ovl = g_cachedOverlayIface.load();
	if (ovl && pv.vrOverlay) {
		mpovr::VCallOvl(ovl, mpovr::SLOT::HideOverlay, "HideOverlay(preview)", pv.vrOverlay);
		mpovr::VCallOvl(ovl, mpovr::SLOT::DestroyOverlay, "DestroyOverlay(preview)", pv.vrOverlay);
	}
	pv.vrOverlay = 0;
	pv.vrShown = false;
}

static bool EnsurePreviewOverlay(void* ovl, Preview& pv, const std::string& fullKey)
{
	if (pv.vrOverlay) return true;
	std::string okey = "prisma_mp_" + std::to_string((uint64_t)std::hash<std::string>{}(fullKey));
	auto err = mpovr::VCallOvl(ovl, mpovr::SLOT::CreateOverlay, "CreateOverlay(preview)",
		okey.c_str(), "Prisma Model Preview", &pv.vrOverlay);
	if (err != mpovr::VROverlayError_None || !pv.vrOverlay) { pv.vrOverlay = 0; return false; }
	mpovr::VCallOvl(ovl, mpovr::SLOT::SetOverlaySortOrder, "SetOverlaySortOrder(preview)",
		pv.vrOverlay, (uint32_t)50);
	return true;
}

static void UpdatePreviewTransform(void* ovl, Preview& pv, const PanelInfo& p)
{
	// Same basis convention as PrismaVR BuildOverlayTransform: right = up x fwd
	float fl = sqrtf(p.fwd[0] * p.fwd[0] + p.fwd[1] * p.fwd[1] + p.fwd[2] * p.fwd[2]);
	if (fl < 1e-6f) fl = 1.0f;
	float fx = p.fwd[0] / fl, fy = p.fwd[1] / fl, fz = p.fwd[2] / fl;
	float ul = sqrtf(p.up[0] * p.up[0] + p.up[1] * p.up[1] + p.up[2] * p.up[2]);
	if (ul < 1e-6f) ul = 1.0f;
	float ux = p.up[0] / ul, uy = p.up[1] / ul, uz = p.up[2] / ul;
	float rx = uy * fz - uz * fy;
	float ry = uz * fx - ux * fz;
	float rz = ux * fy - uy * fx;
	ux = fy * rz - fz * ry;
	uy = fz * rx - fx * rz;
	uz = fx * ry - fy * rx;

	float texW = (float)(p.texWidth ? p.texWidth : 1);
	float texH = (float)(p.texHeight ? p.texHeight : 1);
	float panelH = p.widthMeters * texH / texW;
	float dx = ((pv.rect.x + pv.rect.w * 0.5f) / texW - 0.5f) * p.widthMeters;
	float dy = (0.5f - (pv.rect.y + pv.rect.h * 0.5f) / texH) * panelH;
	float px = p.pos[0] + rx * dx + ux * dy + fx * 0.005f;
	float py = p.pos[1] + ry * dx + uy * dy + fy * 0.005f;
	float pz = p.pos[2] + rz * dx + uz * dy + fz * 0.005f;
	float previewW = (pv.rect.w / texW) * p.widthMeters;
	if (previewW < 0.05f) previewW = 0.05f;

	mpovr::HmdMatrix34_t m;
	m.m[0][0] = rx; m.m[0][1] = ux; m.m[0][2] = fx; m.m[0][3] = px;
	m.m[1][0] = ry; m.m[1][1] = uy; m.m[1][2] = fy; m.m[1][3] = py;
	m.m[2][0] = rz; m.m[2][1] = uz; m.m[2][2] = fz; m.m[2][3] = pz;

	mpovr::VCallOvl(ovl, mpovr::SLOT::SetOverlayWidthInMeters, "SetOverlayWidthInMeters(preview)",
		pv.vrOverlay, previewW);
	mpovr::VCallOvl(ovl, mpovr::SLOT::SetOverlayTransformAbsolute, "SetOverlayTransformAbsolute(preview)",
		pv.vrOverlay, (int)mpovr::TrackingUniverseStanding, &m);
}

void TickVR(void* overlayIface, PanelInfoFn lookup)
{
	if (!Enabled()) return;
	if (overlayIface) g_cachedOverlayIface.store(overlayIface);
	if (!overlayIface || !g_d3dReady) return;

	int shown = 0;
	for (auto& [key, pv] : g_previews) {
		PanelInfo panel;
		bool havePanel = lookup && lookup(pv.viewId, panel) && panel.valid;
		bool want = pv.model && pv.tex && havePanel && shown < MAX_VR_PREVIEWS;
		if (!want) {
			if (pv.vrShown && pv.vrOverlay) {
				mpovr::VCallOvl(overlayIface, mpovr::SLOT::HideOverlay, "HideOverlay(preview)", pv.vrOverlay);
				pv.vrShown = false;
			}
			continue;
		}
		if (!EnsurePreviewOverlay(overlayIface, pv, key)) continue;
		UpdatePreviewTransform(overlayIface, pv, panel);
		mpovr::Texture_t tex{ pv.tex.Get(), mpovr::TextureType_DirectX, mpovr::ColorSpace_Auto };
		mpovr::VCallOvl(overlayIface, mpovr::SLOT::SetOverlayTexture, "SetOverlayTexture(preview)",
			pv.vrOverlay, &tex);
		if (!pv.vrShown) {
			mpovr::VCallOvl(overlayIface, mpovr::SLOT::ShowOverlay, "ShowOverlay(preview)", pv.vrOverlay);
			pv.vrShown = true;
		}
		shown++;
	}
}

// ============================================================================
// Section J: public API / orchestration
// ============================================================================

void Show(uint64_t viewId, const std::string& jsonArgs)
{
	if (!Enabled()) return;

	Request r;
	r.viewId = viewId;
	r.id = JsonGetString(jsonArgs, "id");
	r.plugin = JsonGetString(jsonArgs, "plugin");
	r.localId = (uint32_t)JsonGetNumber(jsonArgs, "localId", 0);
	r.formId = (uint32_t)JsonGetNumber(jsonArgs, "formId", 0);
	r.rect.x = (float)JsonGetNumber(jsonArgs, "x", 0);
	r.rect.y = (float)JsonGetNumber(jsonArgs, "y", 0);
	r.rect.w = (float)JsonGetNumber(jsonArgs, "w", 0);
	r.rect.h = (float)JsonGetNumber(jsonArgs, "h", 0);
	r.zoom = (float)JsonGetNumber(jsonArgs, "zoom", 1.0);
	r.panX = (float)JsonGetNumber(jsonArgs, "panX", 0.0);
	r.panY = (float)JsonGetNumber(jsonArgs, "panY", 0.0);
	r.flip = JsonGetNumber(jsonArgs, "flip", 0.0) != 0.0;
	r.roll = (float)JsonGetNumber(jsonArgs, "roll", 0.0);
	r.spin = (float)JsonGetNumber(jsonArgs, "spin", -10000.0);
	r.yaw = (float)JsonGetNumber(jsonArgs, "yaw", -10000.0); // sentinel: absent = framework keeps/banks the angle
	r.pitch = (float)JsonGetNumber(jsonArgs, "pitch", 0.0);
	r.brightness = (float)JsonGetNumber(jsonArgs, "brightness", 1.0);
	if (r.zoom < 0.25f) r.zoom = 0.25f;
	if (r.zoom > 4.0f) r.zoom = 4.0f;
	if (r.brightness < 0.2f) r.brightness = 0.2f;
	if (r.brightness > 2.5f) r.brightness = 2.5f;

	if ((r.plugin.empty() && !r.formId) || r.rect.w <= 0 || r.rect.h <= 0) {
		logger::warn("ModelPreview: Show with invalid args: {}", jsonArgs);
		return;
	}

	std::string fullKey = MakeFullKey(viewId, r.id);
	std::lock_guard<std::mutex> lock(g_reqMutex);
	g_pendingShows[fullKey] = std::move(r);
}

void Hide(uint64_t viewId, const std::string& jsonArgs)
{
	if (!Enabled()) return;
	std::string id = JsonGetString(jsonArgs, "id");
	std::lock_guard<std::mutex> lock(g_reqMutex);
	// No id = clear every preview in this view; an id clears just that slot.
	g_pendingHides.push_back(id.empty() ? MakeFullKey(viewId, "*") : MakeFullKey(viewId, id));
}

uint64_t ActiveViewId()
{
	for (auto& [k, pv] : g_previews)
		if (pv.viewId) return pv.viewId;
	return 0;
}

void OnPanelDestroyed(uint64_t viewId)
{
	if (!g_cfg.loaded || !g_cfg.enabled || viewId == 0) return;
	std::lock_guard<std::mutex> lock(g_reqMutex);
	g_pendingHides.push_back(MakeFullKey(viewId, "*"));
}

static void ErasePreview(std::map<std::string, Preview>::iterator& it)
{
	DestroyPreviewOverlay(it->second);
	it = g_previews.erase(it);
}

void TickCore(ID3D11Device* dev, ID3D11DeviceContext* ctx)
{
	if (!Enabled() || !dev || !ctx) return;
	g_devForWorker.store(dev);

	// 1. Drain pending edits posted from the JS thread.
	std::vector<std::string> hides;
	std::vector<std::pair<std::string, Request>> shows;
	{
		std::lock_guard<std::mutex> lock(g_reqMutex);
		hides.swap(g_pendingHides);
		shows.reserve(g_pendingShows.size());
		for (auto& kv : g_pendingShows) shows.emplace_back(kv.first, std::move(kv.second));
		g_pendingShows.clear();
	}

	// 2. Apply hides (exact slot, or "<viewId>\x1f*" = the whole view).
	for (auto& h : hides) {
		bool wholeView = h.size() >= 2 && h.back() == '*' && h[h.size() - 2] == '\x1f';
		if (wholeView) {
			std::string prefix = h.substr(0, h.size() - 1);  // keep the \x1f separator
			for (auto it = g_previews.begin(); it != g_previews.end(); ) {
				if (it->first.rfind(prefix, 0) == 0) ErasePreview(it);
				else ++it;
			}
		} else {
			auto it = g_previews.find(h);
			if (it != g_previews.end()) ErasePreview(it);
		}
	}

	if (!EnsureRenderResources(dev)) return;

	// 3. Apply shows: create or update each preview, queue a load if the item changed.
	for (auto& [fullKey, r] : shows) {
		Preview& pv = g_previews[fullKey];
		pv.viewId = r.viewId;
		pv.id = r.id;
		pv.rect = r.rect;
		bool contentChanged = pv.model && (pv.zoom != r.zoom || pv.panX != r.panX || pv.panY != r.panY ||
			pv.roll != r.roll || pv.pitch != r.pitch || pv.brightness != r.brightness ||
			pv.flip != r.flip || pv.spin != r.spin);
		pv.zoom = r.zoom; pv.panX = r.panX; pv.panY = r.panY;
		pv.roll = r.roll; pv.pitch = r.pitch; pv.brightness = r.brightness; pv.flip = r.flip;

		// Spin transitions preserve the visible angle (pause/resume bank in place) unless
		// the caller pins yaw, which means it owns the rotation (grab-and-tumble).
		std::string itemKey = MakeKey(r);
		const bool sameItem = pv.modelKey == itemKey && pv.model;
		const bool yawProvided = r.yaw > -9999.0f;
		const bool wasSpinning = EffSpin(pv.spin) != 0.0f;
		const bool willSpin = EffSpin(r.spin) != 0.0f;
		float oldYaw = pv.yaw;
		if (yawProvided) pv.yaw = r.yaw;
		else if (sameItem && wasSpinning && !willSpin) pv.yaw = pv.lastAngleDeg;
		else if (sameItem && !wasSpinning && willSpin) { pv.yaw = pv.lastAngleDeg; pv.spinStart = std::chrono::steady_clock::now(); pv.spinStarted = true; }
		else if (!sameItem) pv.yaw = 0.0f;
		pv.spin = r.spin;
		if (pv.model && (contentChanged || pv.yaw != oldYaw)) pv.dirty = true;

		if (!sameItem && pv.wantKey != itemKey) {
			pv.wantKey = itemKey;
			pv.loadGen++;
			EnsurePreviewTarget(pv);
			PostJob(fullKey, pv.loadGen, r);
		}
	}

	// 4. Drain completed loads and route each to its preview (drop superseded ones).
	std::deque<LoadResult> results;
	{
		std::lock_guard<std::mutex> lock(g_resultMutex);
		results.swap(g_results);
	}
	for (auto& res : results) {
		auto it = g_previews.find(res.fullKey);
		if (it == g_previews.end()) continue;
		Preview& pv = it->second;
		if (res.loadGen != pv.loadGen) continue;  // a newer request superseded this load
		pv.wantKey.clear();
		if (res.ok && res.model && EnsurePreviewTarget(pv) && ComputePose(pv, res.model, res.key, res.kind)) {
			pv.dirty = true;
		} else {
			pv.model = nullptr;
			pv.modelKey.clear();
			logger::warn("ModelPreview: no drawable preview for '{}'", res.key);
		}
	}

	// 5. Render previews that need it: static ones once (dirty), spinning ones every frame.
	bool anyRender = false;
	for (auto& [k, pv] : g_previews) {
		if (pv.model && pv.rtv && (pv.dirty || EffSpin(pv.spin) != 0.0f)) { anyRender = true; break; }
	}
	if (anyRender) {
		ScopedStateBackup backup(ctx);
		for (auto& [k, pv] : g_previews) {
			if (!pv.model || !pv.rtv) continue;
			if (!pv.dirty && EffSpin(pv.spin) == 0.0f) continue;
			RenderPreviewInner(ctx, pv);
			pv.dirty = false;
		}
	}
}

void Shutdown()
{
	{
		std::lock_guard<std::mutex> lock(g_jobMutex);
		g_workerExit.store(true);
		g_jobs.clear();
	}
	g_jobCv.notify_one();
	if (g_workerStarted && g_worker.joinable()) g_worker.join();
	g_workerStarted = false;

	void* ovl = g_cachedOverlayIface.exchange(nullptr);
	for (auto& [k, pv] : g_previews) {
		if (ovl && pv.vrOverlay)
			mpovr::VCallOvl(ovl, mpovr::SLOT::DestroyOverlay, "DestroyOverlay(preview)", pv.vrOverlay);
	}
	g_previews.clear();

	g_lru.clear();
	g_texCache.clear();
	{
		std::lock_guard<std::mutex> lock(g_resultMutex);
		g_results.clear();
	}
	g_layouts.clear();
	for (auto& vs : g_vs) vs.Reset();
	for (auto& blob : g_vsBlob) blob.Reset();
	g_ps.Reset();
	g_graySrv.Reset();
	g_blendStateAdditive.Reset();
	g_blendStateAlpha.Reset();
	g_blendState.Reset();
	g_depthStateNoWrite.Reset();
	g_depthState.Reset();
	g_rasterizer.Reset();
	g_sampler.Reset();
	g_cb.Reset();
	g_dsv.Reset();
	g_depthTex.Reset();
	g_d3dReady = false;
	g_dev = nullptr;
	g_devForWorker.store(nullptr);

	logger::info("ModelPreview: shutdown complete");
}

// ============================================================================
// Debug: RT dump (bDumpRT=1) - raw BGRA copy via staging texture, saved as .bmp
// ============================================================================

static void DumpRTOnce(ID3D11DeviceContext* ctx, Preview& pv)
{
	static std::string lastDumpKey;
	if (!pv.tex || pv.modelKey == lastDumpKey) return;
	lastDumpKey = pv.modelKey;

	D3D11_TEXTURE2D_DESC td;
	pv.tex->GetDesc(&td);
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> staging;
	if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &staging))) return;
	ctx->CopyResource(staging.Get(), pv.tex.Get());

	D3D11_MAPPED_SUBRESOURCE mapped;
	if (FAILED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return;

	int w = (int)td.Width, h = (int)td.Height;
	BITMAPFILEHEADER bfh = {};
	BITMAPINFOHEADER bih = {};
	bfh.bfType = 0x4D42;
	bfh.bfOffBits = sizeof(bfh) + sizeof(bih);
	bfh.bfSize = bfh.bfOffBits + w * h * 4;
	bih.biSize = sizeof(bih);
	bih.biWidth = w;
	bih.biHeight = -h;
	bih.biPlanes = 1;
	bih.biBitCount = 32;

	FILE* f = nullptr;
	fopen_s(&f, ".\\Data\\SKSE\\Plugins\\PrismaUI_ModelPreview_dump.bmp", "wb");
	if (f) {
		fwrite(&bfh, sizeof(bfh), 1, f);
		fwrite(&bih, sizeof(bih), 1, f);
		for (int y = 0; y < h; ++y)
			fwrite((uint8_t*)mapped.pData + y * mapped.RowPitch, 4, w, f);
		fclose(f);
		logger::info("ModelPreview: dumped RT for '{}'", pv.modelKey);
	}
	ctx->Unmap(staging.Get(), 0);
}

}
