import { MotionGlobalConfig } from "motion/react";

// Unit tests check content and states, not motion. happy-dom cancels running animations when a view
// unmounts, which surfaces as unhandled AbortErrors; the end-to-end tests cover the animated page.
MotionGlobalConfig.skipAnimations = true;
