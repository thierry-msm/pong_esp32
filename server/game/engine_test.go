package game

import "testing"

func TestToggleReadyLaunchesBall(t *testing.T) {
	engine := NewEngine()

	engine.PlayerConnected(2)
	engine.ToggleReady("left", true)
	engine.ToggleReady("right", true)

	if engine.Status != "playing" {
		t.Fatalf("expected status playing, got %q", engine.Status)
	}

	if engine.BallVX == 0 {
		t.Fatalf("expected ball horizontal speed to be non-zero")
	}

	if engine.timerTicks != 0 {
		t.Fatalf("expected initial serve without pending timer, got %d ticks", engine.timerTicks)
	}
}

func TestToggleReadyDoesNotStartWithoutLivePair(t *testing.T) {
	engine := NewEngine()

	engine.PlayerConnected(2)
	engine.ToggleReady("left", false)
	engine.ToggleReady("right", false)

	if engine.Status != "waiting_ready" {
		t.Fatalf("expected status waiting_ready, got %q", engine.Status)
	}

	if engine.BallVX != 0 {
		t.Fatalf("expected ball horizontal speed to stay zero, got %f", engine.BallVX)
	}
}
