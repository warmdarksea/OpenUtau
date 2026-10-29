using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using OpenUtau.Core.Render;
using OpenUtau.Core.Ustx;
using Xunit;

namespace OpenUtau.Core.DawIntegration {
    /// <summary>
    /// The one test that is not run against a stand-in. Everything else here drives
    /// <see cref="DawManager"/> against <see cref="DawTestPlugin"/>, and the plugin repository's
    /// tests drive its session against a fake OpenUtau — two implementations that each agree with
    /// their own fake can still disagree with each other, and nothing else would notice.
    ///
    /// Opt-in, because it needs the C++ plugin built and running. Start its standalone host with a
    /// scratch discovery directory, then point this at the same directory:
    ///
    /// <code>
    /// bridge-host.exe --dir /tmp/bridge-live --rate 48000
    /// OPENUTAU_BRIDGE_DISCOVERY=/tmp/bridge-live dotnet test --filter DawRealPluginTest
    /// </code>
    ///
    /// Skipped when that variable is unset, so CI and everyday runs are unaffected.
    /// </summary>
    [Collection(DawIntegrationCollection.Name)]
    public class DawRealPluginTest : IDisposable {
        /// <summary>Bounded, unlike the ramp the other tests use: the plugin's peak meter is read
        /// as a number here, so the signal has to mean something at the far end.</summary>
        private const float Level = 0.25f;

        /// <summary>An edit that is not a notification, i.e. something that changed the document.</summary>
        private sealed class FakeEdit : UCommand {
            public override void Execute() { }
            public override void Unexecute() { }
            public override string ToString() => "fake edit";
        }

        private DawManager? manager;

        public DawRealPluginTest() { }

        public void Dispose() => manager?.Dispose();

        /// <summary>The minimal singer phrase construction needs, as in <c>DawConformanceTest</c>.</summary>
        class TestSinger : USinger {
            private readonly string displayName;
            public TestSinger(string name) {
                displayName = name;
                found = true;
                loaded = true;
            }
            public override string Id => "test-singer";
            public override string Name => displayName;
            public override System.Collections.Generic.IList<USubbank> Subbanks => new USubbank[0];
            public override bool TryGetOto(string phoneme, out UOto oto) {
                oto = UOto.OfDummy(phoneme);
                return true;
            }
        }

        /// <summary>
        /// One track at unity and centre, one part from 2000 ms to 3000 ms. Project defaults are
        /// 4/4 at 120 bpm with 480 ticks per beat, so 480 ticks is 500 ms: the part sits at ticks
        /// 1920..2880. <c>FilePath</c> is set because <c>DawManager.OpenConnectionAsync</c>
        /// serializes the project for the init handshake, and the USTX serializer refuses an
        /// unsaved project (see <c>DawConformanceTest.BuildProject</c>).
        /// </summary>
        /// <remarks>
        /// The part carries a real phrase, exactly like <c>DawConformanceTest.BuildProject</c>:
        /// <c>DawAudio.TryExtractPart</c> extracts through the part's render request, so a part
        /// seeded only into the planner (as this fixture originally was) is skipped as unrendered
        /// and the layout goes out empty — the plugin then has nothing to pull and the peak meter
        /// below never moves.
        /// </remarks>
        private static UProject BuildProject() {
            var built = new UProject();
            built.FilePath = Path.Combine(Path.GetTempPath(), "real-plugin-test.ustx");
            // The expression descriptors phrase construction reads.
            built.RegisterExpression(new UExpressionDescriptor("engine", "eng", 0, 100, 0) { options = new[] { "" } });
            built.RegisterExpression(new UExpressionDescriptor("volume", "vol", 0, 100, 100));
            built.RegisterExpression(new UExpressionDescriptor("velocity", "vel", 0, 100, 100));
            built.RegisterExpression(new UExpressionDescriptor("modulation", "mod", 0, 100, 0));
            built.RegisterExpression(new UExpressionDescriptor("direct", "dir", 0, 100, 0));
            built.RegisterExpression(new UExpressionDescriptor("shift", "shft", 0, 100, 0));
            built.RegisterExpression(new UExpressionDescriptor("attack", "atk", 0, 100, 100));
            built.RegisterExpression(new UExpressionDescriptor("decay", "dec", 0, 100, 100));
            built.tracks.Clear();
            built.tracks.Add(new UTrack("Live") { TrackNo = 0, Volume = 0, Pan = 0 });
            built.tracks[0].Singer = new TestSinger("Live Singer");
            var live = new UVoicePart { name = "Live A", trackNo = 0, position = 1920, duration = 960 };
            built.parts.Add(live);
            built.timeAxis.BuildSegments(built);
            AddPhrase(built, built.tracks[0], live, 0, 960);
            // Seed the transport's slot registry: one Ready slot of constant level
            // covering the part's 2000 ms .. 3000 ms range, under the phrase's hash.
            var planner = PlaybackManager.Inst.MixPlanner;
            planner.BeginSession(new[] {
                new MixPlanner.SlotSpec(live, 0, live.renderPhrases[0].hash, 2000, 1000, 1),
            });
            planner.RegisterPcm(live, live.renderPhrases[0].hash, 2000, 1000, 1,
                Enumerable.Repeat(Level, 44100).ToArray());
            return built;
        }

        static void AddPhrase(UProject project, UTrack track, UVoicePart part, int pos, int dur) {
            var note = UNote.Create();
            note.position = pos;
            note.duration = dur;
            note.tone = 60;
            note.lyric = "a";
            note.ExtendedDuration = dur;
            part.notes.Add(note);
            var phoneme = new UPhoneme { position = pos, phoneme = "A", Parent = note };
            part.phonemes.Add(phoneme);
            phoneme.Validate(new ValidateOptions(), project, track, part, note);
            if (phoneme.Error) {
                throw new Exception($"phoneme failed to validate: {phoneme.ErrorException}");
            }
            part.renderPhrases.AddRange(RenderPhrase.FromPart(project, track, part));
        }

        [Fact]
        public async Task RealPluginCompletesTheHandshakeAndPullsAudio() {
            string? directory = Environment.GetEnvironmentVariable("OPENUTAU_BRIDGE_DISCOVERY");
            if (string.IsNullOrEmpty(directory)) {
                // Opt-in live test: needs a running bridge-host publishing its discovery file.
                // xUnit v3's runtime skip keeps CI unaffected without disguising the skip as a pass.
                Assert.Skip(
                    "Set OPENUTAU_BRIDGE_DISCOVERY to a running bridge-host's discovery " +
                    "directory to run this live handshake test.");
            }
            Assert.True(Directory.Exists(directory), $"No such directory: {directory}");

            // Discovery is exercised for real: the file was written by the C++ side, and this is
            // the shipping scanner reading it rather than a fixture.
            var servers = new DawServerFinder(directory!).Scan();
            var server = Assert.Single(servers);
            Assert.True(server.IsCompatible, $"Advertised api {server.Info.ApiVersion}.");

            var project = BuildProject();
            manager = new DawManager(null, null, null) {
                UseTimerPump = false,
                ProjectSource = () => project,
            };

            await manager.ConnectAsync(server);

            Assert.Equal(DawConnectionState.Connected, manager.State);
            Assert.True(manager.IsConnected);

            // The plugin pulls what the layout named, on its own worker thread. Serving those
            // requests is the manager's job, so the wait is for the pulls to stop arriving —
            // there is no message that says "I have everything" (§6.2).
            await Task.Delay(TimeSpan.FromSeconds(3));

            Assert.True(manager.IsConnected, "The plugin dropped the connection mid-run.");
            Assert.Equal(DawConnectionState.Connected, manager.State);

            // The last message of the flow, in the direction only the plugin can start it: an edit
            // arms the debounce, and a looping bridge-host reports a fresh start every time the
            // transport jumps back, which §7 says makes everything pending due at once.
            //
            // Arming races the loop on purpose: a wrap's playbackStarted can flush the pending
            // sync between the edit and the check, which is the correct §7 behavior, not a
            // failure. Re-arm when that happens; the loop wraps at most once per few seconds,
            // so consecutive instant flushes mean something is actually wrong.
            bool observedArmed = false;
            for (int attempt = 0; attempt < 3 && !observedArmed; attempt++) {
                manager.OnNext(new FakeEdit(), false);
                await manager.PumpOnceAsync();
                observedArmed = manager.Scheduler.HasPending;
                if (!observedArmed) {
                    await Task.Delay(200);
                }
            }
            Assert.True(observedArmed, "The edit did not arm the debounce.");

            Assert.True(await WaitUntilAsync(() => !manager!.Scheduler.HasPending),
                "No playbackStarted arrived, so the pending sync was never flushed. " +
                "Run bridge-host with --loop for this half of the flow.");

            // Whether the audio actually landed is visible at the other end, in bridge-host's peak
            // meter: one track at unity and centre, so the constant-power pan law puts
            // 0.25 * 0.7071 = 0.1768 in each channel between 2 s and 3 s, and silence either side.
            await manager.DisconnectAsync();

            Assert.False(manager.IsConnected);
        }

        /// <summary>Polls a condition the plugin's own timing decides. False if it never held.</summary>
        private static async Task<bool> WaitUntilAsync(Func<bool> ready) {
            for (int i = 0; i < 100; i++) {
                if (ready()) {
                    return true;
                }
                await Task.Delay(100);
            }
            return false;
        }
    }
}
