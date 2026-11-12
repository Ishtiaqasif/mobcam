import { useEvent } from 'expo';
import { StatusBar } from 'expo-status-bar';
import { useCallback, useEffect, useState } from 'react';
import {
  type LayoutChangeEvent,
  Linking,
  Modal,
  PermissionsAndroid,
  Pressable,
  ScrollView,
  StyleSheet,
  Text,
  View,
} from 'react-native';

import {
  MobcamPreview,
  MobcamStream,
  RESOLUTIONS,
  type MobcamStatus,
  type PairedPc,
  type PairingRequest,
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

      {/* Scrolls on short screens so notices and Paired PCs below the lenses stay reachable. */}
      <ScrollView style={styles.panelScroll} contentContainerStyle={styles.panel}>
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
        {status.notice ? <Text style={styles.notice}>{status.notice}</Text> : null}

        <PairedPcsButton />
      </ScrollView>

      <PairingPrompt />
    </View>
  );
}

/** "Allow PC-NAME to use the camera? Code 482 913" for unpaired PCs on Wi-Fi. */
function PairingPrompt() {
  const [request, setRequest] = useState<PairingRequest | null>(() => MobcamStream.getPendingPairing());

  useEffect(() => {
    const opened = MobcamStream.addListener('onPairingRequest', setRequest);
    const closed = MobcamStream.addListener('onPairingClosed', ({ id }) =>
      setRequest((r) => (r?.id === id ? null : r)),
    );
    return () => {
      opened.remove();
      closed.remove();
    };
  }, []);

  if (!request) return null;
  const answer = (allow: boolean) => {
    MobcamStream.respondPairing(request.id, allow);
    setRequest(null);
  };

  return (
    <Modal transparent animationType="fade" onRequestClose={() => answer(false)}>
      <View style={styles.scrim}>
        <View style={styles.dialog}>
          <Text style={styles.dialogTitle}>Allow {request.pcName} to use the camera?</Text>
          <Text style={styles.muted}>Check that your PC shows the same code:</Text>
          <Text style={styles.code}>{request.code}</Text>
          <Text style={styles.hint}>
            Only allow PCs you own. The PC asks you to confirm the code too. You can remove it later under Paired PCs.
          </Text>
          <View style={styles.dialogButtons}>
            <Pressable style={[styles.dialogButton, styles.denyButton]} onPress={() => answer(false)}>
              <Text style={styles.switchText}>Deny</Text>
            </Pressable>
            <Pressable style={styles.dialogButton} onPress={() => answer(true)}>
              <Text style={styles.switchText}>Allow</Text>
            </Pressable>
          </View>
        </View>
      </View>
    </Modal>
  );
}

function usePairedPcs(): PairedPc[] {
  const [pcs, setPcs] = useState<PairedPc[]>(() => MobcamStream.getPairedPcs());
  useEffect(() => {
    const sub = MobcamStream.addListener('onPairedPcsChanged', () => setPcs(MobcamStream.getPairedPcs()));
    return () => sub.remove();
  }, []);
  return pcs;
}

function PairedPcsButton() {
  const pcs = usePairedPcs();
  const [open, setOpen] = useState(false);

  return (
    <>
      <Pressable onPress={() => setOpen(true)} style={({ pressed }) => pressed && styles.switchButtonPressed}>
        <Text style={styles.link}>Paired PCs ({pcs.length})</Text>
      </Pressable>
      {open && (
        <Modal transparent animationType="fade" onRequestClose={() => setOpen(false)}>
          <View style={styles.scrim}>
            <View style={styles.dialog}>
              <Text style={styles.dialogTitle}>Paired PCs</Text>
              <Text style={styles.hint}>
                These PCs can use the camera over Wi-Fi without asking. USB never needs pairing.
              </Text>
              <ScrollView style={styles.pcList}>
                {pcs.length === 0 ? (
                  <Text style={styles.muted}>None yet.</Text>
                ) : (
                  pcs.map((pc) => (
                    <View key={pc.id} style={styles.pcRow}>
                      <View style={styles.pcInfo}>
                        <Text style={styles.connectionText}>{pc.name}</Text>
                        <Text style={styles.hint}>Paired {new Date(pc.pairedAt).toLocaleDateString()}</Text>
                      </View>
                      <Pressable onPress={() => MobcamStream.forgetPc(pc.id)}>
                        <Text style={styles.forget}>Forget</Text>
                      </Pressable>
                    </View>
                  ))
                )}
              </ScrollView>
              {/* In a row like Allow/Deny: dialogButton's flex: 1 would collapse it in the column. */}
              <View style={styles.dialogButtons}>
                <Pressable style={styles.dialogButton} onPress={() => setOpen(false)}>
                  <Text style={styles.switchText}>Done</Text>
                </Pressable>
              </View>
            </View>
          </View>
        </Modal>
      )}
    </>
  );
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
  panelScroll: { width: 280, flexGrow: 0, backgroundColor: '#15181d' },
  panel: {
    paddingHorizontal: 20,
    paddingVertical: 16,
    gap: 18,
  },
  title: { color: '#fff', fontSize: 22, fontWeight: '700' },
  block: { gap: 4 },
  label: { color: '#8a8f98', fontSize: 12, textTransform: 'uppercase', letterSpacing: 0.6 },
  mono: { color: '#e6e8eb', fontSize: 16, fontFamily: 'monospace' },
  muted: { color: '#8a8f98', fontSize: 14 },
  hint: { color: '#5f646c', fontSize: 12, marginTop: 4 },
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
  notice: { color: '#f5b942', fontSize: 13 },
  link: { color: '#6f9bff', fontSize: 14, fontWeight: '600' },
  scrim: {
    flex: 1,
    backgroundColor: 'rgba(0,0,0,0.6)',
    alignItems: 'center',
    justifyContent: 'center',
    padding: 16,
  },
  dialog: {
    width: 380,
    maxWidth: '100%',
    maxHeight: '100%',
    backgroundColor: '#1c2026',
    borderRadius: 16,
    padding: 20,
    gap: 10,
  },
  dialogTitle: { color: '#fff', fontSize: 18, fontWeight: '700' },
  code: {
    color: '#fff',
    fontSize: 34,
    fontWeight: '700',
    fontFamily: 'monospace',
    letterSpacing: 2,
    textAlign: 'center',
    marginVertical: 4,
  },
  dialogButtons: { flexDirection: 'row', gap: 10, marginTop: 6 },
  dialogButton: {
    flex: 1,
    backgroundColor: '#2b6fff',
    borderRadius: 12,
    paddingVertical: 12,
    alignItems: 'center',
  },
  denyButton: { backgroundColor: '#3a3f47' },
  pcList: { flexGrow: 0 },
  pcRow: { flexDirection: 'row', alignItems: 'center', paddingVertical: 8, gap: 12 },
  pcInfo: { flex: 1 },
  forget: { color: '#ff5a5f', fontWeight: '600' },
  gate: { flex: 1, alignItems: 'center', justifyContent: 'center', gap: 14, padding: 24 },
});
