import csv
from pathlib import Path
import tempfile
import unittest

from measurement import analyze,production_environment,validate_render_environment,runtime_file


class FrameObservations(unittest.TestCase):
    def test_control_cannot_silently_claim_hd_support(self):
        manifest={'control034':{'file':'control034/Macross30Recompiled.exe'}}
        self.assertEqual(runtime_file(manifest,'base034',0),'control034/Macross30Recompiled.exe')
        self.assertEqual(runtime_file(manifest,'candidate035',1),'bin/Macross30Recompiled.exe')
        with self.assertRaisesRegex(ValueError,'no HD'):runtime_file(manifest,'base034',1)
    def test_actual_published_launcher_keeps_optimizations_and_conservative_policy(self):
        environment,proof=production_environment()
        self.assertEqual(environment['LD_TEXTURE_HASH'],'xxh3')
        self.assertEqual(environment['LD_ASYNC_FRAME_RESOURCES'],'1')
        self.assertEqual(environment['EDGE_ANIM'],'0')
        self.assertEqual(environment['RIBBON_NATIVE'],'0')
        self.assertEqual(environment['MACROSS_NATIVE_ZLIB'],'0')
        self.assertEqual(environment['SCREEN_AS_TEX'],'0')
        self.assertTrue(proof['archive_sha256'])
        self.assertNotIn('MACROSS_RUN_SECONDS',environment)
        self.assertNotIn('PAD_FILE',environment)
        self.assertNotIn('PS3_FTRACE',environment)

    def test_missing_or_unsafe_render_policy_stops_before_launch(self):
        environment,_=production_environment()
        del environment['LD_TEXTURE_HASH']
        with self.assertRaisesRegex(ValueError,'LD_TEXTURE_HASH'):validate_render_environment(environment)
        environment,_=production_environment();environment['EDGE_ANIM']='1'
        with self.assertRaisesRegex(ValueError,'EDGE_ANIM'):validate_render_environment(environment)
        environment,_=production_environment();environment['SCREEN_AS_TEX']='1'
        with self.assertRaisesRegex(ValueError,'SCREEN_AS_TEX'):validate_render_environment(environment)

    def test_warmup_and_coalesced_frames_do_not_pollute_percentiles(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'frames.csv'
            with path.open('w',newline='') as stream:
                writer=csv.writer(stream)
                writer.writerow(['frame','frame_delta','qpc','frequency','observed_interval_ms'])
                for row in [(1,1,0,1000,999),(2,1,1000,1000,30),(4,2,1100,1000,1000),
                            (5,1,1200,1000,40),(6,1,2000,1000,888)]:writer.writerow(row)
            result=analyze(path,1,1)
            self.assertEqual(result['sample_count'],2)
            self.assertEqual(result['coalesced_observations'],1)
            self.assertEqual(result['p99_ms'],40)
            self.assertTrue(result['window_complete'])

    def test_short_capture_is_not_reported_as_complete(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'frames.csv'
            path.write_text('frame,frame_delta,qpc,frequency,observed_interval_ms\n1,1,0,1000,33\n2,1,100,1000,34\n')
            self.assertFalse(analyze(path,0,60)['window_complete'])

    def test_no_observations_in_window_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'frames.csv'
            path.write_text('frame,frame_delta,qpc,frequency,observed_interval_ms\n1,1,0,1000,33\n')
            with self.assertRaisesRegex(ValueError,'No single-frame'):analyze(path,20,60)


if __name__=='__main__':unittest.main()
