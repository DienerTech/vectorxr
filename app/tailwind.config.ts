import type { Config } from 'tailwindcss'

export default {
  content: ['./index.html', './src/**/*.{vue,ts}'],
  theme: {
    extend: {
      colors: {
        depthxr: {
          ink: '#111316',
          fog: '#eff2eb',
          sand: '#d7c7a6',
          copper: '#b96f3d',
          steel: '#5f6f7e',
          pine: '#24322d',
        },
      },
      boxShadow: {
        panel: '0 22px 70px rgba(17, 19, 22, 0.14)',
      },
      borderRadius: {
        '4xl': '2rem',
      },
    },
  },
  plugins: [],
} satisfies Config
