SANITIZED_IMAGE=$(echo "$DOCKER_IMAGE" | sed 's/[\/:]/_/g')

if [ "$PR_NUMBER" -eq 0 ]; then
    BASE_PREFIX="REFs/$GITHUB_REF_NAME/$COMMIT_SHA"
else
    BASE_PREFIX="PRs/$PR_NUMBER/$COMMIT_SHA"
fi

GRYPE_PREFIX="$BASE_PREFIX/grype/$SANITIZED_IMAGE"
SYFT_PREFIX="$BASE_PREFIX/scan/$SANITIZED_IMAGE/syft"
GRANT_PREFIX="$BASE_PREFIX/scan/$SANITIZED_IMAGE/grant"

S3_GRYPE_PATH="s3://$S3_BUCKET/$GRYPE_PREFIX"
S3_SYFT_PATH="s3://$S3_BUCKET/$SYFT_PREFIX"
S3_GRANT_PATH="s3://$S3_BUCKET/$GRANT_PREFIX"
HTTPS_RESULTS_PATH="https://$S3_BUCKET.s3.amazonaws.com/index.html#$GRYPE_PREFIX/"
HTTPS_REPORT_PATH="https://s3.amazonaws.com/$S3_BUCKET/$GRYPE_PREFIX/results.html"
echo "https_report_path=$HTTPS_REPORT_PATH" >> $GITHUB_OUTPUT

tfs --no-colors transform nice raw.log nice.log.txt
tfs --no-colors report results -a $HTTPS_RESULTS_PATH raw.log - --copyright "Altinity LTD" | tfs --no-colors document convert > results.html

aws s3 cp --no-progress nice.log.txt $S3_GRYPE_PATH/nice.log.txt --content-type "text/plain; charset=utf-8" || echo "nice log file not found".
aws s3 cp --no-progress results.html $S3_GRYPE_PATH/results.html || echo "results file not found".
aws s3 cp --no-progress raw.log $S3_GRYPE_PATH/raw.log || echo "raw.log file not found".
aws s3 cp --no-progress result.json $S3_GRYPE_PATH/result.json --content-type "text/plain; charset=utf-8" || echo "result.json not found".

aws s3 cp --no-progress "${VERSION}.spdx.json" "$S3_SYFT_PATH/${VERSION}.spdx.json" --content-type "application/json" || echo "${VERSION}.spdx.json not found".
aws s3 cp --no-progress "${VERSION}.cdx.json" "$S3_SYFT_PATH/${VERSION}.cdx.json" --content-type "application/json" || echo "${VERSION}.cdx.json not found".

aws s3 cp --no-progress "${SANITIZED_IMAGE}-licenses.json" "$S3_GRANT_PATH/${SANITIZED_IMAGE}-licenses.json" --content-type "application/json" || echo "${SANITIZED_IMAGE}-licenses.json not found".
