// jsdom has no ResizeObserver (@xyflow/react's canvas needs one to size itself) or real layout, so
// jsdom can never fire the "measured" callback that flips a node from `visibility: hidden` to
// visible — a real-browser-only transient. A no-op stub is enough to stop the ReferenceError; tests
// that need to find node contents by role query with `{ hidden: true }` to see past that CSS state
// (getByText already ignores it, which is why it needs no such flag).
class ResizeObserverStub {
  observe() {}
  unobserve() {}
  disconnect() {}
}
(globalThis as unknown as { ResizeObserver: unknown }).ResizeObserver = ResizeObserverStub;
