export type PrismaViewId = bigint | number | string;

export interface CreateViewOptions {
  id: PrismaViewId;
  url?: unknown;
  order?: unknown;
  hidden?: unknown;
}

export interface PrismaShellApi {
  createView(options: CreateViewOptions): true;
  destroyView(id: PrismaViewId): boolean;
  setHidden(id: PrismaViewId, hidden: unknown): boolean;
  setOrder(id: PrismaViewId, order: unknown): boolean;
  focusView(id: PrismaViewId): boolean;
  blurView(id: PrismaViewId): boolean;
}

declare global {
  interface Window {
    __prismaShell?: PrismaShellApi;
  }
}
