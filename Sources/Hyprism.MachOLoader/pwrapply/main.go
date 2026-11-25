// pwrapply is the part of butler a launcher needs to install and update a game from wharf .pwr
// patches (the format Hytale ships builds in), without the rest of butler, whose SQLite database
// needs cgo and so cannot be built for Android with the plain Go toolchain.
//
// It takes butler's own command line, so a launcher can run it in place of butler:
//
//	pwrapply version
//	pwrapply apply --staging-dir <dir> [--save-interval=<s>] [--signature <sig>] <patch.pwr> <old> [--dir <new>]
//
// Without --dir the patch is applied in place to <old> (the staging dir holds new files until the
// commit). Progress goes to stdout as "<n.nn>%" lines.
package main

import (
	"context"
	"encoding/gob"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/itchio/headway/state"
	"github.com/itchio/lake/pools/fspool"
	"github.com/itchio/savior/filesource"
	"github.com/itchio/wharf/pwr"
	"github.com/itchio/wharf/pwr/bowl"
	"github.com/itchio/wharf/pwr/patcher"

	_ "github.com/itchio/wharf/decompressors/brotli"
	_ "github.com/itchio/wharf/decompressors/gzip"
)

const version = "pwrapply 1.0 (wharf apply)"

type params struct {
	patch, old, dir, staging, signature string
	saveInterval                        time.Duration
}

func main() {
	if len(os.Args) >= 2 && os.Args[1] == "version" {
		fmt.Println(version)
		return
	}
	if len(os.Args) < 2 || os.Args[1] != "apply" {
		usage()
	}
	p := params{saveInterval: 60 * time.Second}
	var positional []string
	args := os.Args[2:]
	for i := 0; i < len(args); i++ {
		a := args[i]
		name, value, hasValue := strings.Cut(a, "=")
		next := func() string {
			if hasValue {
				return value
			}
			i++
			if i >= len(args) {
				usage()
			}
			return args[i]
		}
		switch name {
		case "--staging-dir":
			p.staging = next()
		case "--dir":
			p.dir = next()
		case "--signature":
			p.signature = next()
		case "--save-interval":
			if d, err := time.ParseDuration(next() + "s"); err == nil {
				p.saveInterval = d
			}
		default:
			if strings.HasPrefix(a, "--") {
				fmt.Fprintln(os.Stderr, "ignoring unknown flag", a)
				continue
			}
			positional = append(positional, a)
		}
	}
	if len(positional) != 2 || p.staging == "" {
		usage()
	}
	p.patch, p.old = positional[0], positional[1]
	if err := apply(context.Background(), p); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

func usage() {
	fmt.Fprintln(os.Stderr, "usage: pwrapply version | pwrapply apply --staging-dir <dir> [--signature <sig>] <patch.pwr> <old> [--dir <new>]")
	os.Exit(2)
}

func apply(ctx context.Context, prm params) error {
	start := time.Now()
	var lastPrint time.Time
	consumer := &state.Consumer{
		OnMessage: func(level string, msg string) {
			if level != "debug" {
				fmt.Printf("%s %s\n", level, msg)
			}
		},
	}

	if err := os.MkdirAll(prm.staging, 0o755); err != nil {
		return err
	}
	if err := os.MkdirAll(prm.old, 0o755); err != nil {
		return err
	}

	source, err := filesource.Open(ctx, prm.patch)
	if err != nil {
		return fmt.Errorf("opening patch: %w", err)
	}
	p, err := patcher.New(source, consumer)
	if err != nil {
		return fmt.Errorf("reading patch: %w", err)
	}

	// A checkpoint lets an interrupted install (the app closed, the phone slept) pick up where it was.
	checkpointPath := filepath.Join(prm.staging, "checkpoint.bwl")
	var checkpoint *patcher.Checkpoint
	if f, err := os.Open(checkpointPath); err == nil {
		c := &patcher.Checkpoint{}
		if gob.NewDecoder(f).Decode(c) == nil {
			checkpoint = c
			fmt.Println("info resuming from checkpoint")
		}
		f.Close()
	}
	lastSave := time.Now()
	p.SetSaveConsumer(&saver{
		shouldSave: func() bool {
			if time.Since(lastPrint) > 500*time.Millisecond {
				lastPrint = time.Now()
				fmt.Printf("%.2f%%\n", p.Progress()*100)
			}
			return time.Since(lastSave) > prm.saveInterval
		},
		save: func(c *patcher.Checkpoint) (patcher.AfterSaveAction, error) {
			lastSave = time.Now()
			tmp := checkpointPath + ".tmp"
			f, err := os.Create(tmp)
			if err != nil {
				return patcher.AfterSaveStop, err
			}
			if err := gob.NewEncoder(f).Encode(c); err != nil {
				f.Close()
				return patcher.AfterSaveStop, err
			}
			if err := f.Close(); err != nil {
				return patcher.AfterSaveStop, err
			}
			return patcher.AfterSaveContinue, os.Rename(tmp, checkpointPath)
		},
	})

	targetPool := fspool.New(p.GetTargetContainer(), prm.old)
	defer targetPool.Close()

	var bwl bowl.Bowl
	if prm.dir != "" {
		bwl, err = bowl.NewFreshBowl(ctx, bowl.FreshBowlParams{
			SourceContainer: p.GetSourceContainer(),
			TargetContainer: p.GetTargetContainer(),
			TargetPool:      targetPool,
			OutputFolder:    prm.dir,
		})
	} else {
		bwl, err = bowl.NewOverlayBowl(bowl.OverlayBowlParams{
			SourceContainer: p.GetSourceContainer(),
			TargetContainer: p.GetTargetContainer(),
			OutputFolder:    prm.old,
			StageFolder:     prm.staging,
		})
	}
	if err != nil {
		return fmt.Errorf("preparing output: %w", err)
	}
	if err := p.Resume(checkpoint, targetPool, bwl); err != nil {
		if errors.Is(err, patcher.ErrStop) {
			return fmt.Errorf("stopped at %.2f%%", p.Progress()*100)
		}
		return fmt.Errorf("patching: %w", err)
	}
	if err := targetPool.Close(); err != nil {
		return fmt.Errorf("closing old files: %w", err)
	}
	if err := bwl.Commit(); err != nil {
		return fmt.Errorf("committing: %w", err)
	}
	os.Remove(checkpointPath)
	out := p.GetSourceContainer()
	fmt.Printf("100.00%%\ninfo patched %d files, %d bytes in %s\n", len(out.Files), out.Size, time.Since(start).Round(time.Second))

	if prm.signature != "" {
		sigSource, err := filesource.Open(ctx, prm.signature)
		if err != nil {
			return err
		}
		defer sigSource.Close()
		sig, err := pwr.ReadSignature(ctx, sigSource)
		if err != nil {
			return fmt.Errorf("reading signature: %w", err)
		}
		dir := prm.dir
		if dir == "" {
			dir = prm.old
		}
		v := &pwr.ValidatorContext{FailFast: true, Consumer: consumer}
		if err := v.Validate(ctx, dir, sig); err != nil {
			return fmt.Errorf("verifying: %w", err)
		}
		fmt.Println("info verified against signature")
	}
	return nil
}

type saver struct {
	shouldSave func() bool
	save       func(c *patcher.Checkpoint) (patcher.AfterSaveAction, error)
}

func (s *saver) ShouldSave() bool { return s.shouldSave() }
func (s *saver) Save(c *patcher.Checkpoint) (patcher.AfterSaveAction, error) {
	return s.save(c)
}
