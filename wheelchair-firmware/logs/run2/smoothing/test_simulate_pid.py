"""Behavior checks for the offline controller, not hardware validation."""
import unittest
from unittest.mock import patch
from pathlib import Path
import numpy as np
import pandas as pd
from simulate_pid import Tuning, RPMSetpointPID, reconstruct, replay, step_experiments, build_parser, WHEEL_CIRCUMFERENCE_M

class ShapingTests(unittest.TestCase):
    def test_edited_tuning_defaults_reach_cli_and_flags_override(self):
        edited=Tuning(high_speed_tau_s=.73,low_speed_tau_s=.07,ki=.2,kd=.1,
                      low_speed_accel_m_s2=4.,high_speed_accel_m_s2=2.,max_jerk_rpm_s2=1200)
        with patch('simulate_pid.Tuning',return_value=edited):
            parser=build_parser()
        args=parser.parse_args([])
        self.assertEqual((args.high_tau,args.low_tau,args.ki,args.kd,args.low_accel,args.high_accel,args.jerk),
                         (.73,.07,.2,.1,4.,2.,1200))
        self.assertEqual(parser.parse_args(['--high-tau','.42']).high_tau,.42)
        self.assertEqual(parser.parse_args(['--high-accel','1.5']).high_accel,1.5)

    def test_linear_acceleration_conversion_and_default_limits(self):
        p=Tuning()
        self.assertEqual(p.low_speed_accel_m_s2,2.85)
        self.assertEqual(p.high_speed_accel_m_s2,2.85)
        self.assertAlmostEqual(p.low_speed_accel_rpm_s*WHEEL_CIRCUMFERENCE_M/60,2.85)
        ctrl=RPMSetpointPID(p);rpm=0.
        for _ in range(400):
            nxt=ctrl.update(rpm,1.,.005)
            linear_accel=(nxt-rpm)/.005*WHEEL_CIRCUMFERENCE_M/60
            self.assertLessEqual(abs(linear_accel),2.85+1e-10)
            rpm=nxt

    def test_run2_reconstructs_recorded_targets(self):
        d=pd.read_csv(Path(__file__).resolve().parents[2]/'run2Wheelchair.csv')
        p=Tuning();speed,turn,demand=reconstruct(d,p)
        np.testing.assert_allclose(speed,d.speed,atol=.000051,rtol=0)
        np.testing.assert_allclose(turn,d.turn,atol=.000051,rtol=0)
        np.testing.assert_allclose(demand[:,0]*p.max_rpm,d.leftTargetRPM,atol=.0001,rtol=0)
        np.testing.assert_allclose(demand[:,1]*p.max_rpm,d.rightTargetRPM,atol=.0001,rtol=0)

    def test_small_step_fast_large_step_slower_and_no_step_overshoot(self):
        # Verify the reference tuning, independent of interactive default edits.
        results=step_experiments(Tuning(high_speed_tau_s=.25,
            low_speed_accel_m_s2=180*WHEEL_CIRCUMFERENCE_M/60,
            high_speed_accel_m_s2=60*WHEEL_CIRCUMFERENCE_M/60))
        self.assertLessEqual(results['small_from_rest']['response_90_s'],.12)
        self.assertGreater(results['large_from_rest']['response_90_s'],1.)
        self.assertGreater(results['small_at_speed']['response_90_s'],results['small_from_rest']['response_90_s'])
        for result in results.values():
            self.assertLess(result['overshoot_rpm'],.01)
            self.assertLess(abs(result['output_rpm'][-1]-result['target_rpm'][-1]),.01)

    def test_reversals_obey_rate_and_jerk_limits(self):
        p=Tuning();ctrl=RPMSetpointPID(p);rpm=0.;prev_accel=0.
        rng=np.random.default_rng(11)
        for k in range(3000):
            dt=float(rng.uniform(.003,.012))
            demand=[1.,-1.,.03,0.][(k//150)%4]
            nxt=ctrl.update(rpm,demand,dt)
            accel=(nxt-rpm)/dt
            self.assertTrue(np.isfinite(nxt))
            self.assertLessEqual(abs(accel),p.low_speed_accel_rpm_s+1e-8)
            self.assertLessEqual(abs(accel-prev_accel)/dt,p.max_jerk_rpm_s2+1e-7)
            rpm=nxt;prev_accel=accel

    def test_wheel_states_are_independent(self):
        time=np.arange(0,2,.005)
        demand=np.column_stack([np.ones(len(time)),np.zeros(len(time))])
        result=replay(time,demand,np.ones(len(time),dtype=bool),Tuning())
        self.assertGreater(result[-1,0],100)
        np.testing.assert_array_equal(result[:,1],0.)

    def test_stop_bypasses_smoothing_and_clears_integral(self):
        p=Tuning(ki=1.);ctrl=RPMSetpointPID(p);rpm=0.
        for _ in range(100):rpm=ctrl.update(rpm,1.,.005)
        self.assertGreater(rpm,0)
        self.assertEqual(ctrl.update(rpm,1.,.005,enabled=False),0)
        self.assertEqual(ctrl.integral,0)
        self.assertEqual(ctrl.update(0.,0.,.005),0)

    def test_invalid_timing_and_nonfinite_inputs_rejected(self):
        for dt in [0.,-.005,.2,float('nan')]:
            with self.assertRaises(ValueError):RPMSetpointPID().update(0.,1.,dt)
        with self.assertRaises(ValueError):RPMSetpointPID().update(0.,float('nan'),.005)

if __name__=='__main__':unittest.main()
