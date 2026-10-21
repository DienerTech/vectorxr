// The English catalog, the source every other language translates. It is
// compiled into the app (the others load on demand) and gives MessageKey its
// keys. One JSON file per area of the app; translations mirror the files.
import about from './about.json' with { type: 'json' }
import app from './app.json' with { type: 'json' }
import bindings from './bindings.json' with { type: 'json' }
import common from './common.json' with { type: 'json' }
import depth from './depth.json' with { type: 'json' }
import errors from './errors.json' with { type: 'json' }
import framegen from './framegen.json' with { type: 'json' }
import health from './health.json' with { type: 'json' }
import home from './home.json' with { type: 'json' }
import layers from './layers.json' with { type: 'json' }
import model from './model.json' with { type: 'json' }
import osd from './osd.json' with { type: 'json' }
import pivot from './pivot.json' with { type: 'json' }
import pivotViews from './pivotViews.json' with { type: 'json' }
import quadviews from './quadviews.json' with { type: 'json' }
import registry from './registry.json' with { type: 'json' }
import settings from './settings.json' with { type: 'json' }
import tray from './tray.json' with { type: 'json' }
import turbo from './turbo.json' with { type: 'json' }
import validation from './validation.json' with { type: 'json' }

export const en = {
  about, app, bindings, common, depth, errors, framegen, health, home, layers, model, osd, pivot, pivotViews, quadviews, registry, settings,
  tray, turbo, validation,
}
