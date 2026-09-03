function toMilliseconds(hours, minutes, seconds, milliseconds) {
  return +hours * 3600000 + +minutes * 60000 + +seconds * 1000 + +milliseconds;
}

function convertTo(srt, currentFps, targetFps) {
  if (currentFps === targetFps) {
    return srt;
  }

  const ratio = currentFps / targetFps;

  return srt.replace(
    /(\d{2,}):(\d{2}):(\d{2})[,.](\d{3})/g,
    (_, hours, minutes, seconds, milliseconds) => {
      const time = Math.round(
        toMilliseconds(hours, minutes, seconds, milliseconds) * ratio,
      );

      const pad = (value, length = 2) => String(value).padStart(length, "0");

      return (
        [
          pad(Math.floor(time / 3600000)),
          pad(Math.floor(time / 60000) % 60),
          pad(Math.floor(time / 1000) % 60),
        ].join(":") + `,${pad(time % 1000, 3)}`
      );
    },
  );
}

// Tolerant parsing for the now/next display: sequence numbers and
// block order are ignored, any line carrying a timing starts a cue.
function parseCues(srt) {
  const timing =
    /^(\d{2,}):(\d{2}):(\d{2})[,.](\d{3})\s*-->\s*(\d{2,}):(\d{2}):(\d{2})[,.](\d{3})/;

  const cues = [];
  for (const block of srt.replace(/\r\n?/g, "\n").split(/\n\n+/)) {
    const lines = block.split("\n");
    const index = lines.findIndex((line) => timing.test(line));
    if (index === -1) {
      continue;
    }
    const match = lines[index].match(timing);
    const text = lines
      .slice(index + 1)
      .join("\n")
      .replace(/<[^>]*>/g, "")
      .trim();
    cues.push({
      start: toMilliseconds(...match.slice(1, 5)),
      end: toMilliseconds(...match.slice(5, 9)),
      text,
    });
  }
  return cues.sort((a, b) => a.start - b.start);
}

export { convertTo, parseCues };
