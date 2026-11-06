# Does a straight chest lean command a turn?

The recording contains clear examples of a turn command during an almost centered backward lean. The first forward lean is comparatively clean. See `direction_coupling.png` for the focused graph.

## Definition and measurements

We select forward chest lean of at least +10°, or backward lean of at most −5°, while lateral chest tilt stays within ±2° of its starting posture. The backward threshold is smaller because the first backward movement only reaches about −7.5°. These gates describe measured posture, not confirmed user intent.

A turn command with magnitude greater than 0.10 is used as a **descriptive reference**: 10% of the normalized turn-command range. It is not a validated acceptable-error limit or a 10° heading error. Only the recording up to the final nonzero wheel target (56.54 s) is included, with stopped rows excluded.

| Selected posture | Selected duration | Median absolute turn command | Time with absolute turn >0.10 |
| --- | ---: | ---: | ---: |
| First forward lean (2–6 s) | 3.445 s | 0% | 0% |
| First backward lean (6–9 s) | 0.625 s | 27.4% | 100% |
| All centered forward samples | 11.335 s | 0% | 22.9% |
| All centered backward samples | 2.705 s | 22.5% | 100% |

These durations are the samples passing the posture gates, not the whole named time windows. Percentages are time fractions from this recording, not independent trial success rates; adjacent samples are strongly correlated. All-recording selections include transitions and should not be interpreted as controlled isolated-lean trials.

## Strongest examples

**First forward lean:** median lateral angle +0.68°. Both wheel targets have a median of +155.4 RPM. Median turn command is zero; its absolute 95th percentile is only 0.0575. There is no substantial turn coupling in this selected part of the initial forward movement.

**First backward lean, 7.790–8.415 s:** median forward/back angle −6.61°, lateral angle +1.07°, and turn command +0.274 (right). Median wheel targets are **−27.0 RPM left and −112.0 RPM right**. Both targets reverse, but their magnitudes are very unequal, so ideal tracking would produce curved reverse travel rather than straight reverse. The median logged right sensor value is 200 counts while left is zero in this selected interval, consistent with the turn command coming from lateral seat loading despite the nearly centered chest.

**Second backward example, 17.905–19.985 s:** lasts 2.08 s, with median forward/back angle −14.25°, lateral angle +0.63°, turn command +0.223, and wheel targets −31.5 / −99.6 RPM. This is a longer example of the same asymmetry.

The positive turn command is the controller's right-positive yaw convention. In reverse it should not be described as a forward-travel heading angle. Neither a commanded turn fraction nor an angle in the `(speed, turn)` plane is a physical trajectory heading.

## Interpretation

This is evidence of cross-axis coupling between approximately straight chest posture and the controller's desired motion, particularly in reverse. It does not establish whether the cause is sensor mechanics, calibration, unequal loading, pelvic motion, or mounting alignment: chest tilt and seat load distribution are different measurements. A 1° chest tilt may also produce some real intended turn input. The large wheel-target inequality is the practical quantity to investigate.

The result is robust to the lateral posture gate: repeating the analysis with gates of ±1°, ±2°, ±3°, and ±5° still finds turn magnitude greater than 0.10 throughout the selected backward samples. Forward results depend more on that gate and include transition samples. See `direction_threshold_sensitivity.csv`.

The phone timing alignment, angle zero, and mounting assumptions are inherited from the run 2 analysis. The approximately 50 ms smoothing and any sensor response lag make short transition examples less conclusive. The two backward examples provide clearer evidence than brief high-turn outliers in the forward selections.

Actual wheel RPM, track width, and actual vehicle trajectory are not recorded here. Therefore this analysis estimates commanded motion consistency, not measured curvature, drift distance, or actual driving accuracy. No firmware changes were made.

For a follow-up collection that separates the possible causes, repeat several forward and backward leans while holding each for a few seconds with chest tilt laterally centered, returning to neutral between holds. Mark the intended direction and record actual wheel speeds if available. This would allow per-trial repeatability and steady-state comparisons rather than relying on movement transitions.

## Outputs

- `direction_coupling.png` / `.svg`: initial movement comparison, centered-posture scatter, and time fractions.
- `direction_metrics.csv`: summary statistics.
- `direction_events.csv`: contiguous posture-selected intervals with absolute turn above 0.10, lasting at least 0.10 s. Very short interruptions can split an interval.
- `direction_threshold_sensitivity.csv`: posture gate sensitivity.
- `analyze_direction.py`: reproduction script; run with numpy, pandas, and matplotlib installed.

The moving wheel imbalance metric in `direction_metrics.csv` is `abs(leftRPM-rightRPM)/abs(leftRPM+rightRPM)`, computed only where the absolute mean target RPM is at least 15. Zero means equal targets; it is not an angular error. The median is about 0.61 for the first backward selection and 0.53 for all selected backward samples.
