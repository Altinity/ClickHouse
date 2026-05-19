@0xa1b2c3d4e5f60002;

struct Inner {
    field1 @0 :Text;
    field2 @1 :Int32;
    specialField @2 :Data;
    newSpecialField @3 :Data;
}

struct Message {
    title @0 :Text;
    inner @1 :Inner;
}
