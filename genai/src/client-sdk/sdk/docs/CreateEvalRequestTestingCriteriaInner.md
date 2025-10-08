# CreateEvalRequestTestingCriteriaInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**type** | **str** | The object type, which is always &#x60;label_model&#x60;. | 
**name** | **str** | The name of the grader. | 
**model** | **str** | The model to use for the evaluation. | 
**input** | [**List[EvalItem]**](EvalItem.md) | The input text. This may include template strings. | 
**labels** | **List[str]** | The labels to classify to each item in the evaluation. | 
**passing_labels** | **List[str]** | The labels that indicate a passing result. Must be a subset of labels. | 
**reference** | **str** | The text being graded against. | 
**operation** | **str** | The string check operation to perform. One of &#x60;eq&#x60;, &#x60;ne&#x60;, &#x60;like&#x60;, or &#x60;ilike&#x60;. | 
**pass_threshold** | **float** | The threshold for the score. | 
**evaluation_metric** | **str** | The evaluation metric to use. One of &#x60;fuzzy_match&#x60;, &#x60;bleu&#x60;, &#x60;gleu&#x60;, &#x60;meteor&#x60;, &#x60;rouge_1&#x60;, &#x60;rouge_2&#x60;, &#x60;rouge_3&#x60;, &#x60;rouge_4&#x60;, &#x60;rouge_5&#x60;, or &#x60;rouge_l&#x60;. | 
**source** | **str** | The source code of the python script. | 
**image_tag** | **str** | The image tag to use for the python script. | [optional] 
**sampling_params** | **object** | The sampling parameters for the model. | [optional] 
**range** | **List[float]** | The range of the score. Defaults to &#x60;[0, 1]&#x60;. | [optional] 

## Example

```python
from openapi_client.models.create_eval_request_testing_criteria_inner import CreateEvalRequestTestingCriteriaInner

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalRequestTestingCriteriaInner from a JSON string
create_eval_request_testing_criteria_inner_instance = CreateEvalRequestTestingCriteriaInner.from_json(json)
# print the JSON string representation of the object
print(CreateEvalRequestTestingCriteriaInner.to_json())

# convert the object into a dict
create_eval_request_testing_criteria_inner_dict = create_eval_request_testing_criteria_inner_instance.to_dict()
# create an instance of CreateEvalRequestTestingCriteriaInner from a dict
create_eval_request_testing_criteria_inner_from_dict = CreateEvalRequestTestingCriteriaInner.from_dict(create_eval_request_testing_criteria_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


