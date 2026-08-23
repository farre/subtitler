function convertTo(srt, currentFps, targetFps) {
  if (currentFps === targetFps) {
    return srt;
  }

  const ratio = currentFps / targetFps;

  return srt.replace(
    /(\d{2,}):(\d{2}):(\d{2})[,.](\d{3})/g,
    (_, hours, minutes, seconds, milliseconds) => {
      const time = Math.round(
        (+hours * 3600000 +
          +minutes * 60000 +
          +seconds * 1000 +
          +milliseconds) *
          ratio,
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

export { convertTo };
