# Step 7 - JavaScript Interop

## Goal

Port `Invoke`, `InteropCall`, and `RegisterJSListener` from JavaScriptCore/Ultralight to CEF while preserving public behavior where CEF supports it cleanly.

## Edit Scope

- `src/PrismaUI/Communication.*`
- CEF app/render-process handler
- CEF browser-process message handling
- shell JavaScript helper code

## Current Behavior To Preserve

- `Invoke(view, script, callback)` evaluates a JS string in the target view and optionally returns a result string.
- `InteropCall(view, functionName, argument)` calls a global JS function in the target view with one string argument.
- `RegisterJSListener(view, name, callback)` exposes a global JS function with that name in the target view. JS calls it with one string argument.
- Existing callback-state variants in API V3 must keep passing caller-owned state back unchanged.

## CEF Design

1. Execute JS in the target iframe's CEF frame:
   - resolve frame by `prisma-view-<id>`
   - use `CefFrame::ExecuteJavaScript` for fire-and-forget calls
   - for `Invoke` with result, use an injected async result bridge because `ExecuteJavaScript` does not synchronously return a value like Ultralight's `EvaluateScript`
2. Add a renderer-process bridge:
   - implement `CefRenderProcessHandler::OnContextCreated`
   - detect frames by name or injected metadata
   - install registered callback functions into the frame global object
   - callbacks send `CefProcessMessage` to the browser process with view ID, callback name, and string payload
3. Add browser-process handling:
   - receive JS callback process messages in `CefClient::OnProcessMessageReceived`
   - look up `(viewId, name)` in the existing `jsCallbacks` map
   - run the stored C++ callback
4. Keep dynamic listener registration:
   - store listener names even before iframe load
   - on frame context creation or DOM ready, install all listeners for that view
   - if a listener is registered after load, send a process message or execute installation JS for that frame
5. Implement `InteropCall` as generated frame-local JS:
   - prefer a small call wrapper that JSON-encodes the string argument
   - avoid manual script concatenation where possible
6. Log JS bridge activity at appropriate levels:
   - listener registration and binding per view ID/name
   - missing target frame
   - process message send/receive failures
   - script evaluation exceptions
   - invoke request ID creation/completion/timeout
   - callback lookup misses

## Result Bridge For Invoke

Use request IDs:

```text
C++ Invoke(view, script, callback)
  -> execute wrapper in target frame
  -> wrapper evaluates script and converts result to string
  -> renderer sends process message: InvokeResult(requestId, result)
  -> browser process completes callback
```

The callback should receive an empty string on errors to match current defensive behavior.

## Acceptance Criteria

- Existing JS listener examples continue to work without changing mod JS code.
- `Invoke` returns string results for simple expressions.
- `InteropCall` calls global functions inside the correct iframe.
- JS callbacks are scoped by view ID and callback name.
- Destroying a view clears pending callbacks for that view.
- Logs are sufficient to diagnose missing frames, missing callbacks, and renderer/browser process message failures.

## Risks

- CEF has separate browser and renderer processes. Any state needed by JS bindings must be sent to the renderer process; direct C++ function pointers cannot cross that boundary.
- Synchronous script return is not native to CEF. The compatibility layer must make `Invoke` callback behavior reliable, but it should not block the render thread.
