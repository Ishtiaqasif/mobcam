// "New version available" check (issue #16). Only with the user's consent (asked on first launch,
// changeable in About), at most once a day: asks GitHub for the latest release's version number and
// page. Nothing is downloaded or installed; the user opens the release page to get the new APK.
import Constants from 'expo-constants';
import { useCallback, useEffect, useState } from 'react';

import { MobcamStream } from './modules/mobcam-stream';

const LATEST_RELEASE_API = 'https://api.github.com/repos/Ishtiaqasif/mobcam/releases/latest';
const RELEASES_PAGE = 'https://github.com/Ishtiaqasif/mobcam/releases/';
const DAY_MS = 24 * 60 * 60 * 1000;
const TICK_MS = 60 * 60 * 1000;

const KEY_CONSENT = 'updates.check'; // 'on' | 'off' | null (not asked yet)
const KEY_LAST_CHECK = 'updates.lastCheck'; // ms since epoch
const KEY_LATEST = 'updates.latestVersion';
const KEY_URL = 'updates.latestUrl';
const KEY_SKIPPED = 'updates.skippedVersion'; // "Later": back for the next version

type Stored = { consent: boolean | null; latest: string; url: string; skipped: string };

function load(): Stored {
  const consent = MobcamStream.getAppSetting(KEY_CONSENT);
  return {
    consent: consent === 'on' ? true : consent === 'off' ? false : null,
    latest: MobcamStream.getAppSetting(KEY_LATEST) ?? '',
    url: MobcamStream.getAppSetting(KEY_URL) ?? '',
    skipped: MobcamStream.getAppSetting(KEY_SKIPPED) ?? '',
  };
}

export function currentVersion(): string {
  return Constants.expoConfig?.version ?? '0.0.0';
}

/** "v1.2.0" or "1.2.0" → [1, 2, 0]. Pre-releases ("1.3.0-rc.1") and anything else → null. */
export function parseVersion(text: unknown): [number, number, number] | null {
  if (typeof text !== 'string') return null;
  const m = /^v?(\d+)\.(\d+)\.(\d+)$/i.exec(text.trim());
  return m ? [Number(m[1]), Number(m[2]), Number(m[3])] : null;
}

export function isNewer(candidate: string, than: string): boolean {
  const a = parseVersion(candidate);
  const b = parseVersion(than);
  if (!a || !b) return false;
  for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i];
  return false;
}

/** The latest published release, or null if GitHub can't be reached or answers unexpectedly. */
async function fetchLatest(): Promise<{ version: string; url: string } | null> {
  try {
    const response = await fetch(LATEST_RELEASE_API, {
      // GitHub requires a User-Agent; it identifies the app and version, nothing about the user.
      headers: { Accept: 'application/vnd.github+json', 'User-Agent': `MobCam/${currentVersion()}` },
    });
    if (!response.ok) return null;
    const json = await response.json();
    const version = parseVersion(json?.tag_name);
    const url = json?.html_url;
    // Only ever open MobCam's own release pages, whatever the response says.
    if (!version || typeof url !== 'string' || !url.startsWith(RELEASES_PAGE)) return null;
    return { version: version.join('.'), url };
  } catch {
    return null;
  }
}

export type Updates = {
  /** null = not asked yet */
  consent: boolean | null;
  /** A newer version than this build, not skipped, or null. */
  available: string | null;
  url: string;
  setConsent: (on: boolean) => void;
  /** "Later": hide the notice until an even newer version. */
  skip: () => void;
};

export function useUpdateCheck(): Updates {
  const [stored, setStored] = useState(load);

  useEffect(() => {
    if (stored.consent !== true) return;
    let live = true;
    const tick = () => {
      const last = Number(MobcamStream.getAppSetting(KEY_LAST_CHECK) ?? 0);
      if (Date.now() - last < DAY_MS) return;
      fetchLatest().then((latest) => {
        // Unreachable (offline, GitHub down): try again at the next hourly tick.
        if (!latest || !live) return;
        MobcamStream.setAppSetting(KEY_LAST_CHECK, String(Date.now()));
        MobcamStream.setAppSetting(KEY_LATEST, latest.version);
        MobcamStream.setAppSetting(KEY_URL, latest.url);
        setStored(load());
      });
    };
    tick();
    const timer = setInterval(tick, TICK_MS);
    return () => {
      live = false;
      clearInterval(timer);
    };
  }, [stored.consent]);

  const setConsent = useCallback((on: boolean) => {
    MobcamStream.setAppSetting(KEY_CONSENT, on ? 'on' : 'off');
    if (on) MobcamStream.setAppSetting(KEY_LAST_CHECK, null); // check right away
    setStored(load());
  }, []);

  const skip = useCallback(() => {
    MobcamStream.setAppSetting(KEY_SKIPPED, MobcamStream.getAppSetting(KEY_LATEST));
    setStored(load());
  }, []);

  const available =
    stored.consent === true && isNewer(stored.latest, currentVersion()) && stored.latest !== stored.skipped
      ? stored.latest
      : null;
  return { consent: stored.consent, available, url: stored.url, setConsent, skip };
}
