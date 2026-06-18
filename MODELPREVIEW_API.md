# 3D Item Preview for PrismaUI views

PrismaUI can render a live 3D model of any game item into a box inside your view. You hand it an item and a rectangle to draw in, and it draws the model there with the real mesh and textures. It does not draw any frame, border, or buttons around it. That part is yours to style however you want.

The same code works in flatscreen and in VR. You do not write anything different for the two.

## Check that it is available first

The feature only exists on PrismaUI builds that ship with it, and only when it is switched on in the ini. Test for it before you call it so your view still works on builds that do not have it:

```js
if (typeof window.__prismaUI_showModelPreview === 'function') {
    // ok to use it
}
```

If the function is not there, just skip the preview and fall back to whatever you want, like a flat icon or nothing at all.

## Show a model

It is one call. Give it the item and where to draw it:

```js
window.__prismaUI_showModelPreview(JSON.stringify({
    plugin: "Skyrim.esm",   // the plugin the item comes from
    localId: 0x12EB7,       // its form id inside that plugin, as a number
    x: 100, y: 200,         // top-left corner of the box, in view pixels
    w: 400, h: 400          // box size, in view pixels
}));
```

Use a roughly square box. The render itself is square, so a tall or wide box just leaves empty space on the sides.

If you already have a full runtime form id instead of plugin plus local id, pass `formId` by itself:

```js
window.__prismaUI_showModelPreview(JSON.stringify({
    formId: 0x00012EB7,
    x: 100, y: 200, w: 400, h: 400
}));
```

## Hide it

```js
window.__prismaUI_hideModelPreview('');
```

It also hides itself when your view is destroyed, so you do not have to clean up when the menu closes.

## The three ways to show it

All three come from the same show call. The only difference is which options you pass.

**Spinning turntable.** This is the default. Pass nothing extra and the model turns at the speed set in the ini:

```js
show({ plugin, localId, x, y, w, h });
```

**Still picture.** Set `spin` to 0 and pick the angle you want it frozen at. Use this when you just want a clean static shot of the item instead of a rotating one:

```js
show({ plugin, localId, x, y, w, h, spin: 0, yaw: 30, pitch: 15 });
```

**Drag to rotate.** Wire up your own pointer or controller events, and feed the angle back in as the user drags. Keep `spin` at 0 and update `yaw` and `pitch` yourself:

```js
let yaw = 0, pitch = 0;
onDrag((dx, dy) => {
    yaw += dx * 0.5;
    pitch = Math.max(-85, Math.min(85, pitch + dy * 0.5));
    show({ plugin, localId, x, y, w, h, spin: 0, yaw, pitch });
});
```

There is no separate flat or sprite mode. A still picture is just the 3D render with the spin turned off. You cannot pull a PNG out of it either. It draws straight to the screen, it does not hand you back an image file.

## Every option

| option | default | what it does |
|---|---|---|
| `plugin` + `localId` | required | the item to show, by plugin name and its form id inside that plugin |
| `formId` | required | alternative to the pair above: one full runtime form id |
| `x`, `y`, `w`, `h` | required | the draw box, in view pixels |
| `spin` | ini value | turn speed in degrees per second. 0 freezes it. Leave it out to use the ini default. |
| `yaw` | kept | base turn angle in degrees. Leave it out and it keeps the current angle, so turning spin on and off does not make the model jump. |
| `pitch` | 0 | up and down tilt in degrees, applied after the turn. Good for drag to tumble. Clamp it yourself to about plus or minus 85. |
| `roll` | 0 | flat in-plane spin in degrees, like rotating a photo. |
| `flip` | 0 | set to 1 to turn the model 180 degrees, for meshes that load in upside down. |
| `zoom` | 1.0 | 0.25 to 4.0. Higher is closer. |
| `panX`, `panY` | 0 | nudge the model around inside the box, in units of its own size, roughly minus 1.5 to 1.5. |
| `brightness` | 1.0 | lighting level, 0.2 to 2.5. Turn it up for dark items like ebony or daedric so they read against the background. |

Calling show again for the same item only updates these view options. It does not reload the model, so it is cheap to call every frame while you are dragging or animating.

## How items get turned the right way up

The framework tries to orient each item the way you would expect to see it, based on what kind of item it is:

- Armor and clothing use the mesh's own inventory marker when it has one. That is the same orientation Skyrim's inventory menu uses. If a mesh has no marker, the piece stands upright instead.
- Weapons stand point up.
- Quivers and ammo stand upright.
- Shields stand facing you.
- Everything else shows the way it was built. Potions, food, books, and clutter already sit right, so they are left alone.

If one item still loads in wrong, that is what `flip` and `roll` are for. Set them for that item and the model turns to match. They stick to whatever you store them against on your side.

## Stopping, pausing, and clearing it

There are two different "stops," and they are different on purpose.

Stop the spinning but keep the model on screen. Call show again with `spin: 0`. The model freezes at the angle it is currently showing:

```js
show({ plugin, localId, x, y, w, h, spin: 0 });
```

Start it spinning again. Call show with a `spin` number. Because you did not pass `yaw`, it picks up from the frozen angle with no jump:

```js
show({ plugin, localId, x, y, w, h, spin: 45 });
```

That is the pause and resume trick: toggling `spin` between 0 and a number freezes and unfreezes in place. If you would rather drive the angle yourself (drag to rotate), pass `yaw` every time and forget about this.

Clear it off the screen entirely. Call hide. The box goes empty and the model is dropped:

```js
window.__prismaUI_hideModelPreview('');
```

You do not have to hide on menu close. When your view is destroyed the preview clears itself. Hide is for when you want it gone while the view stays open, like the user clicking away from an item.

## VR: let the user grab the model itself

In VR, holding the trigger normally grabs and moves the whole panel. If you want the user to be able to grab the model in the box instead, put `cursor: grab` (or `cursor: move`) on your preview element in CSS:

```css
#myPreviewBox { cursor: grab; }
```

PrismaVR sees that and sends the trigger hold to your element's mouse handlers instead of grabbing the panel, the same way it already handles sliders and scrollbars. Without it, a long hold on the preview will pick up the panel.

## A full example

This is the whole thing wired to a list of items. The user clicks a row, the model shows in a fixed box, and it clears when they go back. This is all the code you need:

```js
// 1. A box in your HTML for the model to draw into.
//    <div id="itemPreview" style="position:absolute; left:600px; top:120px; width:400px; height:400px;"></div>

function hasPreview() {
    return typeof window.__prismaUI_showModelPreview === 'function';
}

function box() {
    const r = document.getElementById('itemPreview').getBoundingClientRect();
    return { x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width), h: Math.round(r.height) };
}

// Show the model for an item the user picked.
function showItem(item) {
    if (!hasPreview()) return;            // older PrismaUI build, just skip it
    const b = box();
    window.__prismaUI_showModelPreview(JSON.stringify({
        plugin: item.plugin,
        localId: item.localId,
        x: b.x, y: b.y, w: b.w, h: b.h,
        spin: 30,                         // gentle turntable
        brightness: 1.3                   // a little brighter than default
    }));
}

// Clear it.
function clearItem() {
    if (hasPreview()) window.__prismaUI_hideModelPreview('');
}

// Hook it to your rows.
for (const row of document.querySelectorAll('.itemRow')) {
    row.addEventListener('click', () => showItem(rowToItem(row)));
}
document.getElementById('backButton').addEventListener('click', clearItem);
```

If you want the user to be able to freeze and spin it, add two buttons that call show again with `spin: 0` and `spin: 30`. If you want drag to rotate, see the drag example up top.

## What it will not do

- Items with no world model show nothing. Some forms, like trap markers and a few script-only items, simply do not have a mesh to draw.
- The lighting is a clean studio look, not the full game shader stack. You get the right shape and textures, but no environment reflections or parallax, and glow effects are drawn in a simplified way.
- Only one preview is on screen at a time across the whole game. The last view to call show owns it. If two views both try to show a preview at once, the most recent call wins.
- When something does not render, the reason is written to PrismaUI.log. Search it for `ModelPreview` and you will see why a given item came up blank.
