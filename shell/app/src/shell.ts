import type { CreateViewOptions, PrismaShellApi, PrismaViewId } from './types';

const VIEW_ID_PATTERN = /^\d+$/;

function normalizeId(id: PrismaViewId): string {
  if (typeof id === 'bigint') {
    return id.toString(10);
  }
  if (typeof id === 'number' && Number.isSafeInteger(id) && id >= 0) {
    return String(id);
  }
  if (typeof id === 'string' && VIEW_ID_PATTERN.test(id)) {
    return id;
  }
  throw new TypeError(`Invalid Prisma view id: ${String(id)}`);
}

function normalizeOrder(order: unknown): number {
  const numericOrder = Number(order);
  return Number.isFinite(numericOrder) ? Math.trunc(numericOrder) : 0;
}

function setHiddenState(frame: HTMLIFrameElement, id: string, hidden: unknown): void {
  const isHidden = Boolean(hidden);
  frame.dataset.hidden = isHidden ? 'true' : 'false';
  frame.hidden = isHidden;
  frame.style.visibility = isHidden ? 'hidden' : 'visible';
  console.info(`PrismaUI shell set hidden id=${id} iframe=${frame.name} hidden=${String(isHidden)} url=${frame.src}`);
}

function syncInputTarget(views: ReadonlyMap<string, HTMLIFrameElement>, focusedId: string | undefined): void {
  for (const [id, frame] of views) {
    frame.style.pointerEvents =
      frame.dataset.hidden === 'true' || (focusedId !== undefined && id !== focusedId) ? 'none' : 'auto';
  }
}

function getFrame(views: ReadonlyMap<string, HTMLIFrameElement>, id: PrismaViewId): HTMLIFrameElement | undefined {
  const key = normalizeId(id);
  const frame = views.get(key);
  if (!frame) {
    console.warn(`PrismaUI shell command ignored missing iframe id=${key} iframe=${key}`);
  }
  return frame;
}

function installFrameEvents(frame: HTMLIFrameElement, id: string): void {
  frame.addEventListener('load', () => {
    console.info(`PrismaUI shell iframe load id=${id} iframe=${frame.name} url=${frame.src}`);
  });
  frame.addEventListener('error', () => {
    console.error(`PrismaUI shell iframe error id=${id} iframe=${frame.name} url=${frame.src}`);
  });
}

function isCreateViewOptions(value: unknown): value is CreateViewOptions {
  return Boolean(value) && typeof value === 'object';
}

export function createPrismaShell(root: HTMLElement): PrismaShellApi {
  const views = new Map<string, HTMLIFrameElement>();
  let focusedId: string | undefined;
  return Object.freeze({
    createView(options: CreateViewOptions): true {
      if (!isCreateViewOptions(options)) {
        throw new TypeError('createView requires an options object');
      }

      const id = normalizeId(options.id);
      const url = String(options.url ?? 'about:blank');
      const order = normalizeOrder(options.order);
      const hidden = Boolean(options.hidden);
      let frame = views.get(id);

      if (!frame) {
        frame = document.createElement('iframe');
        frame.className = 'prisma-shell-frame';
        frame.id = id;
        frame.name = id;
        frame.dataset.viewId = id;
        frame.allow = 'autoplay *; clipboard-read *; clipboard-write *; encrypted-media *; fullscreen *; web-share *';
        frame.setAttribute('allowtransparency', 'true');
        installFrameEvents(frame, id);
        views.set(id, frame);
        console.info(`PrismaUI shell iframe created id=${id} iframe=${id} url=${url} order=${String(order)} hidden=${String(hidden)}`);
      } else {
        console.info(`PrismaUI shell iframe updated id=${id} iframe=${id} url=${url} order=${String(order)} hidden=${String(hidden)}`);
      }

      frame.style.zIndex = String(order);
      frame.dataset.order = String(order);
      setHiddenState(frame, id, hidden);
      syncInputTarget(views, focusedId);
      if (frame.getAttribute('src') !== url) {
        frame.setAttribute('src', url);
        root.appendChild(frame);
      }
      return true;
    },

    destroyView(id: PrismaViewId): boolean {
      const key = normalizeId(id);
      const frame = views.get(key);
      if (!frame) {
        console.info(`PrismaUI shell destroy ignored missing iframe id=${key} iframe=${key}`);
        return false;
      }

      const { src: url } = frame;
      frame.remove();
      views.delete(key);
      if (focusedId === key) {
        focusedId = undefined;
        syncInputTarget(views, focusedId);
      }
      console.info(`PrismaUI shell iframe destroyed id=${key} iframe=${key} url=${url}`);
      return true;
    },

    setHidden(id: PrismaViewId, hidden: unknown): boolean {
      const key = normalizeId(id);
      const frame = getFrame(views, key);
      if (!frame) {
        return false;
      }
      setHiddenState(frame, key, hidden);
      if (focusedId === key && Boolean(hidden)) {
        focusedId = undefined;
      }
      syncInputTarget(views, focusedId);
      return true;
    },

    setOrder(id: PrismaViewId, order: unknown): boolean {
      const key = normalizeId(id);
      const frame = getFrame(views, key);
      if (!frame) {
        return false;
      }
      const zIndex = normalizeOrder(order);
      frame.style.zIndex = String(zIndex);
      frame.dataset.order = String(zIndex);
      console.info(`PrismaUI shell set order id=${key} iframe=${frame.name} order=${String(zIndex)} url=${frame.src}`);
      return true;
    },

    focusView(id: PrismaViewId): boolean {
      const key = normalizeId(id);
      const frame = getFrame(views, key);
      if (!frame) {
        return false;
      }
      focusedId = key;
      syncInputTarget(views, focusedId);
      frame.focus();
      console.info(`PrismaUI shell iframe focused id=${key} iframe=${frame.name} url=${frame.src}`);
      return true;
    },

    blurView(id: PrismaViewId): boolean {
      const key = normalizeId(id);
      const frame = getFrame(views, key);
      if (!frame) {
        return false;
      }
      if (document.activeElement === frame) {
        frame.blur();
      }
      if (focusedId === key) {
        focusedId = undefined;
        syncInputTarget(views, focusedId);
      }
      console.info(`PrismaUI shell iframe blurred id=${key} iframe=${frame.name} url=${frame.src}`);
      return true;
    },
  });
}
