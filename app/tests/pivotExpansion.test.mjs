import assert from 'node:assert/strict'
import test from 'node:test'
import {defaultConfig,createPivotProfile,normalizeConfig} from '../src/lib/model.ts'
import {validateConfig} from '../src/lib/validation.ts'
import {readFileSync} from 'node:fs'
test('expanded Pivot settings round-trip for default and custom profiles', () => {
  const config=defaultConfig(), motion=config.modules.pivotxr.defaults
  motion.applyMotionToSnapViews=true;motion.deadzoneTransitionDegrees=4;motion.pitchDeadzoneTransitionDegrees=2
  motion.yawLeft.deadzoneTransitionDegrees=7
  motion.positionAssist.enabled=true;motion.positionAssist.advancedDirections=true
  Object.assign(motion.positionAssist.yaw,{rightCm:10,upCm:4,startAngleDegrees:10,fullAngleDegrees:80})
  Object.assign(motion.positionAssist.pitchUp,{upCm:12,forwardCm:-5})
  config.modules.pivotxr.profiles.push(createPivotProfile(motion))
  const restored=normalizeConfig(JSON.parse(JSON.stringify(config)))
  assert.deepEqual(restored.modules.pivotxr.defaults,motion)
  assert.deepEqual(restored.modules.pivotxr.profiles[0].settings,motion)
  restored.modules.pivotxr.profiles[0].settings.positionAssist.yaw.rightCm=22
  assert.equal(restored.modules.pivotxr.defaults.positionAssist.yaw.rightCm,10)
  assert.deepEqual(validateConfig(restored),[])
})
test('old Pivot configuration retains exclusive behavior and hard deadzone', () => {
  const config=defaultConfig(),s=config.modules.pivotxr.defaults
  delete s.applyMotionToSnapViews;delete s.positionAssist;delete s.deadzoneTransitionDegrees;delete s.pitchDeadzoneTransitionDegrees
  for(const key of ['yawLeft','yawRight','pitchUp','pitchDown'])delete s[key].deadzoneTransitionDegrees
  const restored=normalizeConfig(config).modules.pivotxr.defaults
  assert.equal(restored.applyMotionToSnapViews,false);assert.equal(restored.positionAssist.enabled,false)
  assert.equal(restored.deadzoneTransitionDegrees,0);assert.equal(restored.yawLeft.deadzoneTransitionDegrees,0)
})
test('Pivot position ranges and nonfinite transition values are rejected before save', () => {
 const config=defaultConfig(),s=config.modules.pivotxr.defaults
 s.positionAssist.yaw.fullAngleDegrees=s.positionAssist.yaw.startAngleDegrees
 s.positionAssist.pitchUp.upCm=101;s.deadzoneTransitionDegrees=Infinity
 const errors=validateConfig(config).join(' ')
 assert.match(errors,/full-offset/);assert.match(errors,/pitchUp.upCm/);assert.match(errors,/deadzoneTransitionDegrees/)
})
test('public schema describes expanded Pivot fields and directions',()=>{
 const schema=JSON.parse(readFileSync(new URL('../../config/vectorxr.schema.json',import.meta.url),'utf8')).$defs
 const settings=Object.values(schema).find(v=>v.properties?.responseMode && v.properties?.rotationMultiplier)
 for(const field of Object.keys(defaultConfig().modules.pivotxr.defaults)) assert.ok(field in settings.properties,field)
 assert.equal(settings.properties.applyMotionToSnapViews.default,false)
 assert.equal(schema.pivotAxisTuning.properties.deadzoneTransitionDegrees.default,0)
})
