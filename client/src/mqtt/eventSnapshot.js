import { getEventPresentation } from './eventPresentation.js';

export function buildEventPresentationSnapshots(
  events,
  previousSnapshots = new Map(),
  nodeById = new Map(),
  nodeDisplayMap = new Map(),
) {
  const nextSnapshots = new Map();
  const safeEvents = Array.isArray(events) ? events : [];

  safeEvents.forEach((event) => {
    if (!event?.msg_id) return;

    const previous = previousSnapshots.get(event.msg_id);
    nextSnapshots.set(
      event.msg_id,
      previous ?? getEventPresentation(event, nodeById, nodeDisplayMap),
    );
  });

  return nextSnapshots;
}
