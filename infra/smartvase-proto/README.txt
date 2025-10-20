Per far rilevare le librerie custom per Protobuf copia e incolla i seguenti file nella cartella libraries di Arduino (cerca sotto ducumenti/Arduino)
I file da copiare sono:

Da nanopb-nanopb-0.4.9.1:

pb.h

pb_common.c e pb_common.h

pb_decode.c e pb_decode.h

pb_encode.c e pb_encode.h

Dalla tua cartella (ex. smartvase-proto (quelli generati)):

smartvase.pb.c

smartvase.pb.h