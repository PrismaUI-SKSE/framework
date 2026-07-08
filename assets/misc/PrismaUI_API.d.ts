/*
 * For modders: TypeScript definitions for the `window.prismaUi` object available inside a PrismaUI view.
 * Reference it from your UI project (e.g. `/// <reference path="PrismaUI_API.d.ts" />` or via tsconfig
 * "include") to get typing and autocompletion. This is the JavaScript-side counterpart to PrismaUI_API.h.
 *
 * PrismaUI injects `window.prismaUi` before your page scripts run, so it is safe to reference on load.
 */

/** RE::ControlMap::UserEventMapping */
interface PrismaUiControlMapping {
    eventID: string;
    inputKey: number;
    modifier: number;
    remappable: boolean;
    linked: boolean;
}

/** Bindings grouped by input device within a single context */
interface PrismaUiControlContextDevices {
    keyboard?: PrismaUiControlMapping[]
    mouse?: PrismaUiControlMapping[]
    gamepad?: PrismaUiControlMapping[]
    /** Non-standard slots (flat virtual keyboard, VR controllers), keyed by device index (e.g., 3) */
    others: { [deviceIndex: number]: PrismaUiControlMapping[] }
}

type PrismaUiControlContextName = "Gameplay" | "MenuMode" | "Console" | "ItemMenu" | "Inventory" | "DebugText" | "Favorites" | "Map" | "Stats" | "Cursor" | "Book" | "DebugOverlay" | "Journal" | "TFCMode" | "MapDebug" | "Lockpicking" | "Favor/Marketplace" | "Marketplace";

/** One input context (Gameplay, MenuMode, Inventory, etc.) and its per-device bindings */
interface PrismaUiControlContext {
    /** Context index in the engine's control map */
    index: number;
    /** Canonical context name, e.g. "Gameplay", "MenuMode", "Inventory" */
    name: string;
    devices: PrismaUiControlContextDevices;
}

/** Snapshot of the game's key bindings (RE::ControlMap). For window.prismaUi.controls.map. */
interface PrismaUiControlMap {
    /** Gamepad glyph set: 0 = Xbox/DirectX, 1 = PlayStation/Orbis */
    gamePadType: number;
    contexts: PrismaUiControlContext[];
}

/** detail of the gamepadbuttondown and gamepadbuttonup events */
interface PrismaUiGamepadButtonEventDetail {
    /** Button index in the W3C Standard Gamepad mapping, matching navigator.getGamepads() */
    w3cButtonIndex: number;
    /** Raw Skyrim gamepad button ID code */
    skyrimIdCode: number;
    /** Menu role of the button in MenuMode: "accept", "cancel", or "" when it maps to neither */
    action: "accept" | "cancel" | "";
}

/** Events dispatched on `window.prismaUi.controls` */
interface PrismaUiControlsEventMap {
    /** Fired when a gamepad button is pressed */
    gamepadbuttondown: CustomEvent<PrismaUiGamepadButtonEventDetail>
    /** Fired when a gamepad button is released */
    gamepadbuttonup: CustomEvent<PrismaUiGamepadButtonEventDetail>
    /** Fired when the control map is refreshed from SKSE (e.g., on load, on focus, or after calling controls.refresh()) */
    refreshcomplete: CustomEvent<PrismaUiControlMap>
}

/**
 * Input subsystem of PrismaUI, exposed as `window.prismaUi.controls`. It is an EventTarget, so use
 * `window.prismaUi.controls.addEventListener(...)` to observe the events in PrismaUiControlsEventMap.
 */
interface PrismaUiControls extends EventTarget {
    /** Latest key-binding snapshot. Populated on load. */
    map?: PrismaUiControlMap

    /** Requests a fresh map snapshot from the game (in case the player remaps controls) */
    refresh(): void

    /** Gets the Skyrim event ID using the W3C Standard Gamepad button index */
    getEventIdByButtonIndex(w3cButtonIndex: number, contextName?: PrismaUiControlContextName): string | null

    addEventListener<K extends keyof PrismaUiControlsEventMap>(type: K, listener: (this: PrismaUiControls, ev: PrismaUiControlsEventMap[K]) => unknown, options?: boolean | AddEventListenerOptions): void
    addEventListener(type: string, listener: EventListenerOrEventListenerObject, options?: boolean | AddEventListenerOptions): void
    removeEventListener<K extends keyof PrismaUiControlsEventMap>(type: K, listener: (this: PrismaUiControls, ev: PrismaUiControlsEventMap[K]) => unknown, options?: boolean | EventListenerOptions): void
    removeEventListener(type: string, listener: EventListenerOrEventListenerObject, options?: boolean | EventListenerOptions): void
}

/** The PrismaUI JavaScript namespace, exposed as `window.prismaUi`. */
interface PrismaUi {
    controls: PrismaUiControls
}

interface Window {
    prismaUi: PrismaUi
}
