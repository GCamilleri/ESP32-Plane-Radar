import type { TagRegistry } from './tag_registry';

export interface Env {
  TAGS: DurableObjectNamespace<TagRegistry>;
  ADSB_BASE: string;
  LOCK_SECONDS: string;
  FEED_CACHE_SECONDS: string;
  TAG_CACHE_SECONDS: string;
  CLAIMS_PER_HOUR: string;
  MAX_ACTIVE_TAGS: string;
}

/** Reply shape shared by every TagRegistry method, rendered as text/plain. */
export interface Reply {
  status: number;
  body: string;
}

export function envNumber(raw: string | undefined, fallback: number): number {
  const n = Number(raw);
  return Number.isFinite(n) ? n : fallback;
}
