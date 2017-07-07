record R {
  var x: int;
  proc init(x:int) {
    this.x = x;
    super.init();
    writeln("Creating ", x);
  }
  proc deinit() {
    writeln("Destroying ", x);
  }
}

iter myIter() {
  var r = new R(0);

  for i in 1..10 {
    var r2 = new R(i);
    yield i;
  }
}

config const breakOnIter = 2;

proc foo() {

  for i in myIter() {
    writeln(i);
    if i == breakOnIter then
      break;
  }
}

foo();
