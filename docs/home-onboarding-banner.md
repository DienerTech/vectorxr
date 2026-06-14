# Future work: Home onboarding banner

Deferred to a separate PR (not part of the Home/About dashboard split).

## Idea

A small, **dismissable** "New here? Read the guide →" banner on the Home tab, aimed at
first-time users. The Home tab is now a pure status dashboard (System Health + Enhancement
matrix), so there's no longer any onboarding/welcome copy there — this banner would fill that
first-run gap without permanently re-cluttering Home.

## Notes / constraints

- **Not permanent.** It should disappear once dismissed, and ideally not return.
- **Persistence:** store the dismissed flag locally (e.g. a `localStorage` key like
  `vectorxr.home.onboardingDismissed`, similar to how theme preference is persisted) rather than
  in `settings.json`, since it's a per-machine UI nicety, not portable config.
- **Target:** "Read the guide" should link to wherever the user guide ends up (README section,
  docs site, or the in-app About tab welcome prose). TBD when written.
- **Placement:** top of the Home tab, above System Health; quiet styling (accent-soft surface),
  with a clear dismiss affordance.
- Keep it lightweight — the whole point of the dashboard redesign was to reduce visual busyness.
