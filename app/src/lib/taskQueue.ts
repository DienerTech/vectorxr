// Runs tasks one at a time in call order; a failed task does not stop the
// next one.
export function createTaskQueue() {
  let tail: Promise<unknown> = Promise.resolve()
  return function queue<T>(task: () => Promise<T>): Promise<T> {
    const run = tail.then(task, task)
    tail = run.catch(() => undefined)
    return run
  }
}
