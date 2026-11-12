import { useEvent } from 'expo';
import Constants from 'expo-constants';
import { StatusBar } from 'expo-status-bar';
import { useCallback, useEffect, useState } from 'react';
import {
  type LayoutChangeEvent,
  Linking,
  PermissionsAndroid,
  Pressable,
  StyleSheet,
  Text,
  View,
} from 'react-native';

import {
  MobcamPreview,
  MobcamStream,
  RESOLUTIONS,
  type MobcamStatus,
} from './modules/mobcam-stream';

type Permission = 'unknown' | 'granted' | 'denied' | 'blocked';

export default function App() {
  const [permission, setPermission] = useState<Permission>('unknown');

  const requestPermission = useCallback(async () => {
    const result = await PermissionsAndroid.request(PermissionsAndroid.PERMISSIONS.CAMERA);
    if (result === PermissionsAndroid.RESULTS.GRANTED) setPermission('granted');
    else if (result === PermissionsAndroid.RESULTS.NEVER_ASK_AGAIN) setPermission('blocked');
    else setPermission('denied');
  }, []);

  useEffect(() => {
    requestPermission();
  }, [requestPermission]);

  return (
    <View style={styles.root}>
      <StatusBar hidden />
      {permission === 'granted' ? (
        <Streaming />
      ) : (
        <PermissionGate state={permission} onRetry={requestPermission} />
      )}
    </View>
  );
}

function Streaming() {
  const status = useEvent(MobcamStream, 'onStatus', MobcamStream.getStatus()) as MobcamStatus;

  useEffect(() => {
    MobcamStream.start();
  }, []);

  const aspect = status.width > 0 && status.height > 0 ? status.width / status.height : 16 / 9;

  // Fit the preview inside its area at the stream's aspect ratio (letterboxed, never stretched).
  const [area, setArea] = useState({ width: 0, height: 0 });
  const onAreaLayout = (e: LayoutChangeEvent) => {
    const { width, height } = e.nativeEvent.layout;
    setArea({ width, height });
  };
  const fitted =
    area.width / Math.max(area.height, 1) > aspect
      ? { width: area.height * aspect, height: area.height }
      : { width: area.width, height: area.width / aspect };

  return (
    <View style={styles.row}>
      <View style={styles.previewArea} onLayout={onAreaLayout}>
        {area.width > 0 && <MobcamPreview style={fitted} />}
      </View>

      <View style={styles.panel}>
        <Text style={styles.title}>MobCam</Text>

        <ConnectionLine status={status} />

        <View style={styles.block}>
          <Text style={styles.label}>Wi-Fi address</Text>
          {status.addresses.length > 0 ? (
            status.addresses.map((ip) => (
              <Text key={ip} style={styles.mono}>
                {ip}:{status.port}
              </Text>
            ))
          ) : (
            <Text style={styles.muted}>Not on a network. Use USB.</Text>
          )}
          <Text style={styles.hint}>Open MobCam on your PC (USB or Wi-Fi)</Text>
        </View>

        <View style={styles.block}>
          <Text style={styles.label}>
            Resolution{status.height > 0 ? `  ·  ${status.width}×${status.height}` : ''}
          </Text>
          <View style={styles.segment}>
            {RESOLUTIONS.map((r) => {
              const selected = status.targetHeight === r;
              return (
                <Pressable
                  key={r}
                  onPress={() => MobcamStream.setResolution(r)}
                  style={[styles.segmentItem, selected && styles.segmentItemOn]}
                >
                  <Text style={[styles.segmentText, selected && styles.segmentTextOn]}>{r}p</Text>
                </Pressable>
              );
            })}
          </View>
        </View>

        <View style={styles.block}>
          <Text style={styles.label}>
            Camera{status.lenses[status.lens] ? `  ·  ${status.lenses[status.lens].label}` : ''}
          </Text>
          {status.lenses.length > 0 ? (
            <View style={styles.lensRow}>
              {status.lenses.map((lens, i) => {
                const selected = status.lens === i;
                return (
                  <Pressable
                    key={`${lens.label}-${i}`}
                    onPress={() => MobcamStream.selectLens(i)}
                    style={[styles.lensChip, selected && styles.segmentItemOn]}
                  >
                    <Text style={[styles.segmentText, selected && styles.segmentTextOn]}>{lens.short}</Text>
                  </Pressable>
                );
              })}
            </View>
          ) : (
            <Pressable
              onPress={() => MobcamStream.switchCamera()}
              style={({ pressed }) => [styles.switchButton, pressed && styles.switchButtonPressed]}
            >
              <Text style={styles.switchText}>⟲  Switch camera</Text>
            </Pressable>
          )}
        </View>

        {status.error ? <Text style={styles.error}>{status.error}</Text> : null}

        <Text style={styles.version}>{versionLabel()}</Text>
      </View>
    </View>
  );
}

/** "MobCam 1.0.0 (1) · e964e90", plus "· modified" for builds with uncommitted changes. */
function versionLabel(): string {
  const config = Constants.expoConfig;
  const extra = (config?.extra ?? {}) as { commit?: string; modified?: boolean };
  const parts = [`MobCam ${config?.version ?? '?'} (${config?.android?.versionCode ?? '?'})`];
  if (extra.commit) parts.push(extra.commit);
  if (extra.modified) parts.push('modified');
  return parts.join(' · ');
}

function ConnectionLine({ status }: { status: MobcamStatus }) {
  let color = '#8a8f98';
  let text = 'Waiting for PC…';
  if (status.error) {
    color = '#ff5a5f';
    text = 'Camera error';
  } else if (status.clients > 0) {
    color = '#3ddc84';
    const via = [status.usbClients > 0 && 'USB', status.wifiClients > 0 && 'Wi-Fi']
      .filter(Boolean)
      .join(' + ');
    text = `Connected (${via})`;
  } else if (!status.active) {
    text = 'Starting…';
  }
  return (
    <View style={styles.connection}>
      <View style={[styles.dot, { backgroundColor: color }]} />
      <Text style={styles.connectionText}>{text}</Text>
    </View>
  );
}

function PermissionGate({ state, onRetry }: { state: Permission; onRetry: () => void }) {
  if (state === 'unknown') return null;
  const blocked = state === 'blocked';
  return (
    <View style={styles.gate}>
      <Text style={styles.title}>Camera access needed</Text>
      <Text style={styles.muted}>MobCam streams your phone camera to your PC.</Text>
      <Pressable
        style={styles.switchButton}
        onPress={blocked ? () => Linking.openSettings() : onRetry}
      >
        <Text style={styles.switchText}>{blocked ? 'Open settings' : 'Allow camera'}</Text>
      </Pressable>
    </View>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: '#0d0f12' },
  row: { flex: 1, flexDirection: 'row' },
  previewArea: {
    flex: 1,
    backgroundColor: '#000',
    alignItems: 'center',
    justifyContent: 'center',
  },
  panel: {
    width: 280,
    paddingHorizontal: 20,
    paddingVertical: 16,
    gap: 18,
    backgroundColor: '#15181d',
  },
  title: { color: '#fff', fontSize: 22, fontWeight: '700' },
  block: { gap: 4 },
  label: { color: '#8a8f98', fontSize: 12, textTransform: 'uppercase', letterSpacing: 0.6 },
  mono: { color: '#e6e8eb', fontSize: 16, fontFamily: 'monospace' },
  muted: { color: '#8a8f98', fontSize: 14 },
  hint: { color: '#5f646c', fontSize: 12, marginTop: 4 },
  version: { color: '#5f646c', fontSize: 11 },
  connection: { flexDirection: 'row', alignItems: 'center', gap: 8 },
  dot: { width: 10, height: 10, borderRadius: 5 },
  connectionText: { color: '#e6e8eb', fontSize: 15 },
  segment: {
    flexDirection: 'row',
    backgroundColor: '#0d0f12',
    borderRadius: 10,
    padding: 3,
    marginTop: 4,
  },
  segmentItem: { flex: 1, paddingVertical: 8, borderRadius: 8, alignItems: 'center' },
  lensRow: {
    flexDirection: 'row',
    flexWrap: 'wrap',
    gap: 6,
    marginTop: 4,
  },
  lensChip: {
    minWidth: 52,
    paddingVertical: 9,
    paddingHorizontal: 12,
    borderRadius: 18,
    alignItems: 'center',
    backgroundColor: '#0d0f12',
  },
  segmentItemOn: { backgroundColor: '#2b6fff' },
  segmentText: { color: '#8a8f98', fontWeight: '600' },
  segmentTextOn: { color: '#fff' },
  switchButton: {
    backgroundColor: '#2b6fff',
    borderRadius: 12,
    paddingVertical: 14,
    alignItems: 'center',
  },
  switchButtonPressed: { opacity: 0.75 },
  switchText: { color: '#fff', fontSize: 16, fontWeight: '600' },
  error: { color: '#ff5a5f', fontSize: 13 },
  gate: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 14, padding: 24 },
});
