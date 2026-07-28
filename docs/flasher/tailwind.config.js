/** @type {import('tailwindcss').Config} */
module.exports = {
  content: ['./index.html', './docs/flasher/index.html'],
  theme: {
    extend: {
      colors: {
        void:   '#08090c',
        slate1: '#12151b',
        ember:  '#ff7a18',
        signal: '#e2231a',
        jade:   '#34d399',
        ash:    '#8b949e',
      },
      fontFamily: {
        display: ['"Space Grotesk"', 'system-ui', 'sans-serif'],
        sans:    ['"IBM Plex Sans"', 'system-ui', 'sans-serif'],
        mono:    ['"IBM Plex Mono"', 'ui-monospace', 'monospace'],
      },
      keyframes: {
        grow:    { from: { transform: 'scaleX(0)' }, to: { transform: 'scaleX(1)' } },
        rise:    { from: { opacity: '0', transform: 'translateY(12px)' }, to: { opacity: '1', transform: 'none' } },
        drift:   { '0%,100%': { transform: 'translate(0,0)' }, '50%': { transform: 'translate(3%,-4%)' } },
      },
      animation: {
        grow:  'grow .55s cubic-bezier(.22,1,.36,1) both',
        rise:  'rise .5s cubic-bezier(.22,1,.36,1) both',
        drift: 'drift 22s ease-in-out infinite',
      },
    },
  },
  plugins: [],
};
