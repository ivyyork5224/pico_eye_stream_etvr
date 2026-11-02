// VRCFaceTracking module for the Pico Neo 3 Pro Eye, hybrid version:
// gaze from Pico's own eye tracker (eyestream on the headset, UDP 7777) and
// smooth eyelids from the EyeTrackVR app reading the eye cameras (OSC, UDP 9010).
//
// Packet: PEYE,seq,time_s,ok,comb_status,l_status,r_status,gx,gy,gz,px,py,pz,l_open,r_open
// gaze is the combined gaze direction in Unity axes (+x right, +y up, +z forward).
//
// ETVR settings this needs: OSC port 9010, "VRCFT v2" output on, pyVRCFT off.
// It then sends /avatar/parameters/v2/EyeLidLeft and EyeLidRight (0 closed .. ~1 open).
// While no ETVR lids arrive, Pico's own open/closed blinks are used instead.
//
// Gaze is smoothed with a One Euro filter: strong smoothing while the eyes hold still (hides
// tracker jitter), little while they move fast (glances don't lag). Tune it with settings.txt
// next to this DLL (VRCFT's CustomLibs\<module id>\), read when VRCFT starts:
//   gaze_smoothing = 1.0    lower = steadier but laggier when still (Hz, 0.1..10)
//   gaze_speed = 3.0        higher = less lag on fast moves (0..10)
//   gaze_smoothing = 0 turns smoothing off.

using System;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using Microsoft.Extensions.Logging;
using VRCFaceTracking;
using VRCFaceTracking.Core.Library;
using VRCFaceTracking.Core.Types;

namespace PicoNeo3EyeEtvrModule
{
    public class PicoNeo3EyeEtvrModule : ExtTrackingModule
    {
        const int Port = 7777;
        const int EtvrPort = 9010;
        // Gaze is sent to VRCFT as tan(angle); clamp to about +-60 degrees
        const float MaxTan = 1.7f;

        // ETVR lid value that counts as fully open (its open eye sits around 0.8-0.95)
        const float LidOpenAt = 0.8f;
        // ETVR lids older than this are stale; fall back to Pico's blinks
        static readonly TimeSpan EtvrTimeout = TimeSpan.FromMilliseconds(500);

        // Pico reports openness as 0 or 1 only; ease towards it so blinks don't snap.
        // Time constants in seconds: closing is quick, opening a little slower.
        const float CloseTau = 0.025f;
        const float OpenTau = 0.045f;

        UdpClient _udp, _etvr;
        Thread _etvrThread;
        DateTime _lastPacket = DateTime.MinValue;
        bool _receiving;
        float _leftOpen = 1f, _rightOpen = 1f;
        // Debounce: an eye only changes open/closed after 2 frames agree, which hides
        // the open/closed flicker Pico reports right as the lid covers the pupil
        readonly Debounce _leftState = new Debounce(), _rightState = new Debounce();
        DateTime _lastSmooth = DateTime.UtcNow;

        // One Euro gaze filter settings (see the top of the file)
        float _gazeMinCutoff = 1.0f, _gazeBeta = 3.0f;
        readonly OneEuro _gazeX = new OneEuro(), _gazeY = new OneEuro();
        DateTime _lastGaze = DateTime.MinValue;

        // written by the ETVR thread
        volatile float _etvrLeft = 1f, _etvrRight = 1f;
        long _etvrTicks;
        bool _usingEtvr;

        public override (bool SupportsEye, bool SupportsExpression) Supported => (true, false);

        public override (bool eyeSuccess, bool expressionSuccess) Initialize(bool eyeAvailable, bool expressionAvailable)
        {
            ModuleInformation.Name = "Pico Neo 3 Eye + ETVR lids";
            try
            {
                _udp = new UdpClient(new IPEndPoint(IPAddress.Any, Port));
                _udp.Client.ReceiveTimeout = 200;
            }
            catch (Exception e)
            {
                Logger.LogError("Pico Neo 3 Eye: can't listen on UDP {Port} ({Message}). Is another Pico eye module or pico_eye_receiver.py running?", Port, e.Message);
                return (false, false);
            }
            try
            {
                _etvr = new UdpClient(new IPEndPoint(IPAddress.Loopback, EtvrPort));
                _etvrThread = new Thread(EtvrLoop) { IsBackground = true, Name = "ETVR lids" };
                _etvrThread.Start();
            }
            catch (Exception e)
            {
                // gaze still works; lids come from Pico
                Logger.LogWarning("Pico Neo 3 Eye: can't listen for ETVR on UDP {Port} ({Message}). Using Pico blinks.", EtvrPort, e.Message);
            }
            LoadSettings();
            Logger.LogInformation("Pico Neo 3 Eye: listening on UDP {Port}, ETVR lids on {EtvrPort}", Port, EtvrPort);
            return (true, false);
        }

        void LoadSettings()
        {
            string path = Path.Combine(Path.GetDirectoryName(typeof(PicoNeo3EyeEtvrModule).Assembly.Location) ?? "", "settings.txt");
            if (File.Exists(path))
            {
                foreach (string line in File.ReadAllLines(path))
                {
                    string[] kv = line.Split('#')[0].Split('=');
                    if (kv.Length != 2 || !float.TryParse(kv[1].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out float v)) continue;
                    switch (kv[0].Trim().ToLowerInvariant())
                    {
                        case "gaze_smoothing": _gazeMinCutoff = Math.Clamp(v, 0f, 10f); break;
                        case "gaze_speed": _gazeBeta = Math.Clamp(v, 0f, 10f); break;
                    }
                }
            }
            Logger.LogInformation("Pico Neo 3 Eye: gaze smoothing {Cutoff}, speed {Beta}{Off}", _gazeMinCutoff, _gazeBeta,
                _gazeMinCutoff <= 0 ? " (off)" : "");
        }

        public override void Update()
        {
            byte[] data;
            try
            {
                IPEndPoint from = null;
                data = _udp.Receive(ref from);
            }
            catch (SocketException)
            {
                // timeout: no packet in 200 ms
                if (_receiving && DateTime.UtcNow - _lastPacket > TimeSpan.FromSeconds(2))
                {
                    _receiving = false;
                    Logger.LogWarning("Pico Neo 3 Eye: no data from headset. Is eyestream running?");
                }
                return;
            }
            catch (ObjectDisposedException)
            {
                Thread.Sleep(100);
                return;
            }

            string[] p = Encoding.ASCII.GetString(data).Trim().Split(',');
            if (p.Length != 15 || p[0] != "PEYE") return;

            var ci = CultureInfo.InvariantCulture;
            if (!float.TryParse(p[7], NumberStyles.Float, ci, out float gx) ||
                !float.TryParse(p[8], NumberStyles.Float, ci, out float gy) ||
                !float.TryParse(p[9], NumberStyles.Float, ci, out float gz) ||
                !float.TryParse(p[13], NumberStyles.Float, ci, out float lo) ||
                !float.TryParse(p[14], NumberStyles.Float, ci, out float ro))
                return;
            bool ok = p[3] == "1";

            _lastPacket = DateTime.UtcNow;
            if (!_receiving)
            {
                _receiving = true;
                Logger.LogInformation("Pico Neo 3 Eye: receiving data");
            }

            var now = DateTime.UtcNow;
            float dt = (float)Math.Min((now - _lastSmooth).TotalSeconds, 0.1);
            _lastSmooth = now;
            _leftOpen = Ease(_leftOpen, _leftState.Update(lo > 0.5f) ? 1f : 0f, dt);
            _rightOpen = Ease(_rightOpen, _rightState.Update(ro > 0.5f) ? 1f : 0f, dt);

            bool etvrFresh = now - new DateTime(Interlocked.Read(ref _etvrTicks), DateTimeKind.Utc) < EtvrTimeout;
            if (etvrFresh != _usingEtvr)
            {
                _usingEtvr = etvrFresh;
                Logger.LogInformation(etvrFresh ? "Pico Neo 3 Eye: using ETVR eyelids" : "Pico Neo 3 Eye: no ETVR eyelids, using Pico blinks");
            }

            var eye = UnifiedTracking.Data.Eye;
            if (etvrFresh)
            {
                eye.Left.Openness = Math.Clamp(_etvrLeft / LidOpenAt, 0f, 1f);
                eye.Right.Openness = Math.Clamp(_etvrRight / LidOpenAt, 0f, 1f);
            }
            else
            {
                eye.Left.Openness = _leftOpen;
                eye.Right.Openness = _rightOpen;
            }

            // Gaze only updates on valid frames; during a blink the eyes keep their last direction
            if (ok && gz > 0.1f)
            {
                float x = Math.Clamp(gx / gz, -MaxTan, MaxTan);
                float y = Math.Clamp(gy / gz, -MaxTan, MaxTan);
                if (_gazeMinCutoff > 0)
                {
                    // after a gap (blink, headset off) start fresh instead of sliding from the old spot
                    float gdt = (float)(now - _lastGaze).TotalSeconds;
                    if (gdt > 0.25f) { _gazeX.Reset(); _gazeY.Reset(); }
                    gdt = Math.Clamp(gdt, 0.001f, 0.1f);
                    x = _gazeX.Filter(x, gdt, _gazeMinCutoff, _gazeBeta);
                    y = _gazeY.Filter(y, gdt, _gazeMinCutoff, _gazeBeta);
                }
                _lastGaze = now;
                var gaze = new Vector2(x, y);
                eye.Left.Gaze = gaze;
                eye.Right.Gaze = gaze;
            }
        }

        // One Euro filter (Casiez et al. 2012): the cutoff frequency rises with speed
        class OneEuro
        {
            const float DerivCutoff = 1.0f;
            bool _has;
            float _x, _dx;

            public void Reset() => _has = false;

            public float Filter(float value, float dt, float minCutoff, float beta)
            {
                if (!_has) { _has = true; _x = value; _dx = 0; return value; }
                _dx = Lerp(_dx, (value - _x) / dt, Alpha(DerivCutoff, dt));
                float cutoff = minCutoff + beta * Math.Abs(_dx);
                _x = Lerp(_x, value, Alpha(cutoff, dt));
                return _x;
            }

            static float Alpha(float cutoff, float dt)
            {
                float tau = 1f / (2f * MathF.PI * cutoff);
                return 1f / (1f + tau / dt);
            }

            static float Lerp(float a, float b, float t) => a + (b - a) * t;
        }

        void EtvrLoop()
        {
            var buf = new byte[0];
            while (_etvr != null)
            {
                try
                {
                    IPEndPoint from = null;
                    buf = _etvr.Receive(ref from);
                    ReadOsc(buf, 0, buf.Length);
                }
                catch (SocketException) { }
                catch (ObjectDisposedException) { return; }
                catch (Exception) { }  // malformed packet
            }
        }

        // OSC message or bundle; only single-float messages for the two lid addresses matter
        void ReadOsc(byte[] b, int start, int end)
        {
            int i = start;
            string addr = ReadString(b, ref i, end);
            if (addr == "#bundle")
            {
                i += 8;  // time tag
                while (i + 4 <= end)
                {
                    int size = (b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3];
                    i += 4;
                    if (size <= 0 || i + size > end) return;
                    ReadOsc(b, i, i + size);
                    i += size;
                }
                return;
            }
            bool left = addr == "/avatar/parameters/v2/EyeLidLeft";
            if (!left && addr != "/avatar/parameters/v2/EyeLidRight") return;
            if (ReadString(b, ref i, end) != ",f" || i + 4 > end) return;
            var raw = new byte[] { b[i + 3], b[i + 2], b[i + 1], b[i] };  // big-endian
            float v = BitConverter.ToSingle(raw, 0);
            if (left) _etvrLeft = v; else _etvrRight = v;
            Interlocked.Exchange(ref _etvrTicks, DateTime.UtcNow.Ticks);
        }

        static string ReadString(byte[] b, ref int i, int end)
        {
            int s = i;
            while (i < end && b[i] != 0) i++;
            string str = Encoding.ASCII.GetString(b, s, i - s);
            i = (i + 4) & ~3;  // skip the terminator and padding to 4 bytes
            return str;
        }

        class Debounce
        {
            bool _state = true, _pending = true;
            int _count;

            public bool Update(bool raw)
            {
                if (raw == _state) { _count = 0; _pending = raw; return _state; }
                _count = raw == _pending ? _count + 1 : 1;
                _pending = raw;
                if (_count >= 2) { _state = raw; _count = 0; }
                return _state;
            }
        }

        static float Ease(float current, float target, float dt)
        {
            float tau = target < current ? CloseTau : OpenTau;
            return current + (target - current) * (1f - MathF.Exp(-dt / tau));
        }

        public override void Teardown()
        {
            _udp?.Close();
            _udp?.Dispose();
            _udp = null;
            var etvr = _etvr;
            _etvr = null;
            etvr?.Close();
            etvr?.Dispose();
        }
    }
}
