"""Registered cards in report order."""
from .cycle import Cycle
from .wide_insert import WideInsert
from .attach_200 import Attach200
from .mutation import Mutation
from .lwd_concurrent import LwdConcurrent
from .select_subset import SelectSubset
from .dup_content import DupContent

CARDS = {card.name: card for card in (Cycle(), WideInsert(), Attach200(), Mutation(), LwdConcurrent(),
                                      SelectSubset(), DupContent())}
