// Bootstrap script injected by the renderer process into every PrismaUI iframe
// context from `PrismaCefRenderApp::OnContextCreated`. It is evaluated as a
// standalone IIFE; it is NOT part of the shell page bundle and is never shipped
// as a file. The renderer first installs the `window.__prismaNative` native
// bridge (including `imeFocusListenerName`) and then evaluates the bundled
// output of this file.
//
// This is the single source of truth for the iframe bootstrap behavior:
//   1. console.* wrapping  -> native.fireConsole(level, text)
//   2. window.__prismaInstallListener(name) trampoline installer
//   3. IME focus tracking  -> native.fireListener(native.imeFocusListenerName, '1'|'0')
//   4. DOM-ready dispatch  -> native.fireDomReady()

interface PrismaNativeBridge {
  fireListener(name: string, value: string): void;
  fireConsole(level: string, text: string): void;
  fireDomReady(): void;
  readonly imeFocusListenerName: string;
}

type PrismaListenerFn = (arg?: unknown) => void;
type PrismaImeFocusNotifyFn = (element: unknown) => void;

declare global {
  interface Window {
    __prismaNative?: PrismaNativeBridge;
    __prismaInstallListener?: (name: string) => void;
    __prismaImeFocusNotify?: PrismaImeFocusNotifyFn;
  }
}

(function () {
  const native = window.__prismaNative;
  if (!native) {
    return;
  }

  try {
    const c = console;
    const orig: Record<string, ((...args: unknown[]) => void) | undefined> = {
      log: c.log ? c.log.bind(c) : undefined,
      info: c.info ? c.info.bind(c) : undefined,
      warn: c.warn ? c.warn.bind(c) : undefined,
      error: c.error ? c.error.bind(c) : undefined,
      debug: c.debug ? c.debug.bind(c) : undefined,
    };
    const stringifyArgs = (args: unknown[]): string => {
      const parts: string[] = [];
      for (let i = 0; i < args.length; i++) {
        const v = args[i];
        try {
          if (v && typeof v === 'object') {
            parts.push(JSON.stringify(v));
          } else {
            parts.push(String(v));
          }
        } catch (_) {
          parts.push(String(v));
        }
      }
      return parts.join(' ');
    };
    const wrap = (level: string) => {
      return (...args: unknown[]): void => {
        try {
          const fn = orig[level];
          if (fn) {
            fn.apply(c, args);
          }
        } catch (_) {}
        try {
          native.fireConsole(level, stringifyArgs(args));
        } catch (_) {}
      };
    };
    c.log = wrap('log');
    c.info = wrap('info');
    c.warn = wrap('warn');
    c.error = wrap('error');
    c.debug = wrap('debug');
  } catch (_) {}

  try {
    window.__prismaInstallListener = (name: string): void => {
      const listener: PrismaListenerFn = (arg?: unknown): void => {
        try {
          native.fireListener(name, arg === undefined || arg === null ? '' : String(arg));
        } catch (_) {}
      };
      (window as unknown as Record<string, unknown>)[name] = listener;
    };
  } catch (_) {}

  try {
    const isTextInputElement = (el: unknown): boolean => {
      if (!el || typeof el !== 'object') {
        return false;
      }
      const node = el as {
        disabled?: unknown;
        readOnly?: unknown;
        isContentEditable?: unknown;
        tagName?: unknown;
        type?: unknown;
      };
      if (node.disabled || node.readOnly) {
        return false;
      }
      if (node.isContentEditable) {
        return true;
      }
      const tag = String(node.tagName || '').toUpperCase();
      if (tag === 'TEXTAREA') {
        return true;
      }
      if (tag !== 'INPUT') {
        return false;
      }
      const type = String(node.type || 'text').toLowerCase();
      switch (type) {
        case '':
        case 'text':
        case 'search':
        case 'url':
        case 'tel':
        case 'password':
        case 'email':
        case 'number':
          return true;
        default:
          return false;
      }
    };
    const notifyImeFocus: PrismaImeFocusNotifyFn = (element: unknown): void => {
      try {
        native.fireListener(native.imeFocusListenerName, isTextInputElement(element) ? '1' : '0');
      } catch (_) {}
    };
    window.__prismaImeFocusNotify = notifyImeFocus;
    document.addEventListener(
      'focusin',
      (event: Event): void => {
        notifyImeFocus(event.target);
      },
      true,
    );
    document.addEventListener(
      'focusout',
      (): void => {
        setTimeout((): void => {
          notifyImeFocus(document.activeElement);
        }, 0);
      },
      true,
    );
    notifyImeFocus(document.activeElement);
  } catch (_) {}

  try {
    const fireReady = (): void => {
      try {
        native.fireDomReady();
      } catch (_) {}
      try {
        if (typeof window.__prismaImeFocusNotify === 'function') {
          window.__prismaImeFocusNotify(document.activeElement);
        }
      } catch (_) {}
    };
    if (document.readyState === 'loading') {
      const once = (): void => {
        document.removeEventListener('DOMContentLoaded', once);
        fireReady();
      };
      document.addEventListener('DOMContentLoaded', once);
    } else {
      fireReady();
    }
  } catch (_) {}
})();

export {};
