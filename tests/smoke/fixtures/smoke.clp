; Smoke-test fixture: one trigger template, one result template, one rule.

(deftemplate ping
   (slot id))

(deftemplate pong
   (slot id)
   (slot status))

(defrule answer-ping
   (ping (id ?id))
   =>
   (assert (pong (id ?id) (status ok))))
