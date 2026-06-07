import './style.css';

import { createPrismaShell } from './shell';

const root = document.getElementById('prisma-root');

if (!(root instanceof HTMLElement)) {
  throw new Error('PrismaUI shell root element is missing');
}

Object.defineProperty(window, '__prismaShell', {
  configurable: false,
  enumerable: false,
  writable: false,
  value: createPrismaShell(root),
});

console.info('PrismaUI CEF shell loaded');
