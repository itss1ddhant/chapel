
use MasonPublish;
use MasonUtils;

proc test(arg: [] string) {
  masonPublish(arg);
}
const res0 = ['mason', 'publish','--help'];

test(res0);


