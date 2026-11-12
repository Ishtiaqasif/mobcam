import { NativeModule, requireNativeModule, requireNativeView } from 'expo';
import type { ComponentType } from 'react';
import type { ViewProps } from 'react-native';

export type Facing = 'back' | 'front';

/** Resolutions offered in the UI; the phone picks the closest camera size at or below this height. */
export const RESOLUTIONS = [480, 720, 1080] as const;
export type Resolution = (typeof RESOLUTIONS)[number];

export type Lens = {
  /** "Ultra-wide", "Wide", "Telephoto", "Front"... */
  label: string;
  /** Chip text: "0.67×", "1×", "3.4×" or "Front". */
  short: string;
  zoom: number;
  front: boolean;
};

export type MobcamStatus = {
  /** Camera is (or should be) streaming and no error is pending. */
  active: boolean;
  facing: Facing;
  /** Index into `lenses` of the lens in use, -1 if unknown. */
  lens: number;
  lenses: Lens[];
  /** Requested max height (480 / 720 / 1080). */
  targetHeight: number;
  /** Actual encoded size; 0 while starting/stopped. */
  width: number;
  height: number;
  error: string | null;
  /** Non-fatal message, e.g. an outdated PC tried to connect. */
  notice: string | null;
  clients: number;
  usbClients: number;
  wifiClients: number;
  /** LAN IPv4 addresses of the phone, Wi-Fi first. */
  addresses: string[];
  port: number;
  /** "Keep streaming in background" setting (persisted on the phone). */
  backgroundStreaming: boolean;
  /** The background service is running (setting on and a PC connected). */
  backgroundActive: boolean;
};

/** An unpaired PC on Wi-Fi asks to use the camera. Both screens show `code`. */
export type PairingRequest = {
  id: number;
  pcName: string;
  /** "482 913" */
  code: string;
};

export type PairedPc = {
  /** SHA-256 of the PC's identity key, hex. */
  id: string;
  name: string;
  /** ms since epoch */
  pairedAt: number;
};

type MobcamEvents = {
  onStatus: (status: MobcamStatus) => void;
  onPairingRequest: (request: PairingRequest) => void;
  /** The prompt was answered, timed out, or the PC went away. */
  onPairingClosed: (event: { id: number }) => void;
  onPairedPcsChanged: () => void;
};

declare class MobcamStreamModule extends NativeModule<MobcamEvents> {
  start(): void;
  switchCamera(): void;
  selectLens(index: number): void;
  setResolution(height: number): void;
  /** Keep streaming with the screen off or another app open, while a PC is connected. */
  setBackgroundStreaming(on: boolean): void;
  getStatus(): MobcamStatus;
  respondPairing(id: number, allow: boolean): void;
  getPendingPairing(): PairingRequest | null;
  getPairedPcs(): PairedPc[];
  forgetPc(id: string): void;
}

export const MobcamStream = requireNativeModule<MobcamStreamModule>('MobcamStream');

/** Live camera preview. Size it with the stream's aspect ratio. */
export const MobcamPreview: ComponentType<ViewProps> = requireNativeView('MobcamStream');
